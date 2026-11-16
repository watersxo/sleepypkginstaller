using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace Sleepy
{
    /// <summary>One game: the base package plus its updates and add-on content.</summary>
    public class GameGroup
    {
        public string TitleId;
        public string Title;
        public string Region = "";
        public PkgInfo Base;
        public List<PkgInfo> Updates = new List<PkgInfo>();   // newest first
        public List<PkgInfo> Dlc = new List<PkgInfo>();
        public List<PkgInfo> Others = new List<PkgInfo>();
        public int Duplicates;
        public long Size;       // base + newest update + all DLC
        public bool Orphan;     // no base package found
        public long Updated;    // newest file modification time
        public string IconPath; // extracted icon0.png, if any

        public IEnumerable<PkgInfo> Parts()
        {
            if (Base != null) yield return Base;
            foreach (var u in Updates) yield return u;
            foreach (var d in Dlc) yield return d;
        }
    }

    public class ScanStatus
    {
        public bool Scanning;
        public int Done, Total, Errors;
    }

    /// <summary>Scans folders for packages and keeps them grouped by game.</summary>
    public class Library
    {
        readonly string _dir;
        readonly object _lock = new object();
        readonly object _scanLock = new object();
        Dictionary<string, PkgInfo> _byId = new Dictionary<string, PkgInfo>();
        Dictionary<string, PkgInfo> _cache = new Dictionary<string, PkgInfo>(StringComparer.OrdinalIgnoreCase);
        List<GameGroup> _groups = new List<GameGroup>();
        ScanStatus _status = new ScanStatus();
        int _version;

        public event Action Changed;

        public Library()
        {
            _dir = ConfigStore.Dir;
            Directory.CreateDirectory(IconDir);
            try
            {
                var f = Path.Combine(_dir, "library.json");
                if (File.Exists(f))
                {
                    var ser = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
                    var list = ser.Deserialize<List<PkgInfo>>(File.ReadAllText(f, Encoding.UTF8));
                    if (list != null) foreach (var p in list) if (p?.Path != null) _cache[p.Path] = p;
                }
            }
            catch { _cache.Clear(); }
        }

        public string IconDir { get { return Path.Combine(_dir, "icons"); } }
        public int Version { get { return Volatile.Read(ref _version); } }
        public ScanStatus Status { get { lock (_lock) return new ScanStatus { Scanning = _status.Scanning, Done = _status.Done, Total = _status.Total, Errors = _status.Errors }; } }
        public List<GameGroup> Groups { get { lock (_lock) return _groups; } }

        public PkgInfo Pkg(string id)
        {
            lock (_lock) { _byId.TryGetValue(id ?? "", out var p); return p; }
        }

        public static string PkgId(string path)
        {
            using (var sha = SHA1.Create())
            {
                var h = sha.ComputeHash(Encoding.UTF8.GetBytes(Path.GetFullPath(path).ToLowerInvariant()));
                return BitConverter.ToString(h, 0, 6).Replace("-", "").ToLowerInvariant();
            }
        }

        string IconFile(string id) { return Path.Combine(IconDir, id + ".png"); }

        /// <summary>Walks the folders, parses new or changed packages and rebuilds the groups.</summary>
        public void Scan(IEnumerable<string> folders)
        {
            if (!Monitor.TryEnter(_scanLock)) return;
            try
            {
                lock (_lock) _status = new ScanStatus { Scanning = true };
                Changed?.Invoke();

                var files = new List<string>();
                foreach (var root in folders)
                {
                    foreach (var f in EnumeratePkgs(root)) files.Add(f);
                    lock (_lock) _status.Total = files.Count;
                }
                files = files.Distinct(StringComparer.OrdinalIgnoreCase).ToList();

                var results = new ConcurrentBag<PkgInfo>();
                int done = 0, errors = 0;
                var opts = new ParallelOptions { MaxDegreeOfParallelism = 8 };
                Parallel.ForEach(files, opts, path =>
                {
                    var info = Load(path);
                    if (info != null) results.Add(info); else Interlocked.Increment(ref errors);
                    int n = Interlocked.Increment(ref done);
                    if (n % 16 == 0)
                        lock (_lock) { _status.Done = n; _status.Errors = errors; _status.Total = files.Count; }
                });

                var list = results.ToList();
                var byId = list.ToDictionary(p => p.Id);
                var groups = BuildGroups(list);

                lock (_lock)
                {
                    _byId = byId;
                    _groups = groups;
                    _cache = list.ToDictionary(p => p.Path, StringComparer.OrdinalIgnoreCase);
                    _status = new ScanStatus { Done = files.Count, Total = files.Count, Errors = errors };
                }
                Interlocked.Increment(ref _version);

                try
                {
                    var ser = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
                    File.WriteAllText(Path.Combine(_dir, "library.json"), ser.Serialize(list), Encoding.UTF8);
                }
                catch { }
                Changed?.Invoke();
            }
            finally { Monitor.Exit(_scanLock); }
        }

        static IEnumerable<string> EnumeratePkgs(string root)
        {
            var stack = new Stack<string>();
            stack.Push(root);
            while (stack.Count > 0)
            {
                var dir = stack.Pop();
                string[] files = null, dirs = null;
                try { files = Directory.GetFiles(dir, "*.pkg"); dirs = Directory.GetDirectories(dir); } catch { }
                if (files != null) foreach (var f in files) yield return f;
                if (dirs != null) foreach (var d in dirs) stack.Push(d);
            }
        }

        PkgInfo Load(string path)
        {
            try
            {
                var fi = new FileInfo(path);
                string id = PkgId(path);
                PkgInfo cached;
                lock (_lock) _cache.TryGetValue(path, out cached);
                if (cached != null && cached.Size == fi.Length && cached.ModTime == fi.LastWriteTimeUtc.Ticks
                    && (!cached.HasIcon || File.Exists(IconFile(id))))
                {
                    cached.Id = id;
                    return cached;
                }

                var info = PkgReader.Read(path, true, out var icon);
                info.Id = id;
                if (icon != null) File.WriteAllBytes(IconFile(id), icon);
                return info;
            }
            catch { return null; }
        }

        List<GameGroup> BuildGroups(List<PkgInfo> pkgs)
        {
            var byTitle = new Dictionary<string, GameGroup>();
            var seen = new Dictionary<string, PkgInfo>();

            foreach (var p in pkgs.OrderBy(x => x.Path, StringComparer.OrdinalIgnoreCase))
            {
                if (!byTitle.TryGetValue(p.TitleId, out var g)) byTitle[p.TitleId] = g = new GameGroup { TitleId = p.TitleId };

                string key = p.ContentId + "|" + p.Version + "|" + p.Kind;
                if (seen.TryGetValue(key, out var prev))
                {
                    g.Duplicates++;
                    // keep the complete copy, then the one with the shorter path
                    if ((prev.Incomplete && !p.Incomplete) || (prev.Incomplete == p.Incomplete && p.Path.Length < prev.Path.Length))
                    {
                        Replace(g, prev, p);
                        seen[key] = p;
                    }
                    continue;
                }
                seen[key] = p;

                switch (p.Kind)
                {
                    case Kinds.Base:
                        if (g.Base == null || BetterBase(p, g.Base)) { if (g.Base != null) g.Others.Add(g.Base); g.Base = p; }
                        else g.Others.Add(p);
                        break;
                    case Kinds.Update: g.Updates.Add(p); break;
                    case Kinds.Dlc: g.Dlc.Add(p); break;
                    default: g.Others.Add(p); break;
                }
            }

            foreach (var g in byTitle.Values)
            {
                g.Updates = g.Updates.OrderByDescending(u => u.Version, Comparer<string>.Create(PkgReader.CompareVersions)).ToList();
                g.Dlc = g.Dlc.OrderBy(d => d.Title, StringComparer.CurrentCultureIgnoreCase).ToList();
                g.Orphan = g.Base == null;

                PkgInfo sample = g.Base ?? g.Updates.FirstOrDefault() ?? g.Dlc.FirstOrDefault() ?? g.Others.FirstOrDefault();
                g.Title = (g.Base ?? sample)?.Title;
                if (string.IsNullOrEmpty(g.Title)) g.Title = g.TitleId;

                if (g.Base != null) g.Size += g.Base.Size;
                if (g.Updates.Count > 0) g.Size += g.Updates[0].Size;
                g.Size += g.Dlc.Sum(d => d.Size);

                if (sample != null && sample.ContentId.Length >= 2) g.Region = RegionOf(sample.ContentId);
                foreach (var p in g.Parts().Concat(g.Others)) if (p.ModTime > g.Updated) g.Updated = p.ModTime;

                foreach (var p in g.Parts())
                    if (p.HasIcon && File.Exists(IconFile(p.Id))) { g.IconPath = IconFile(p.Id); break; }
            }

            return byTitle.Values.OrderBy(g => g.Title, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        static void Replace(GameGroup g, PkgInfo old, PkgInfo repl)
        {
            if (g.Base == old) g.Base = repl;
            for (int i = 0; i < g.Updates.Count; i++) if (g.Updates[i] == old) g.Updates[i] = repl;
            for (int i = 0; i < g.Dlc.Count; i++) if (g.Dlc[i] == old) g.Dlc[i] = repl;
            for (int i = 0; i < g.Others.Count; i++) if (g.Others[i] == old) g.Others[i] = repl;
        }

        // prefer a complete file, then the newest version
        static bool BetterBase(PkgInfo a, PkgInfo b)
        {
            if (a.Incomplete != b.Incomplete) return !a.Incomplete;
            return PkgReader.CompareVersions(a.Version, b.Version) > 0;
        }

        static string RegionOf(string contentId)
        {
            switch (contentId.Substring(0, 2))
            {
                case "UP": return "US";
                case "EP": return "EU";
                case "JP": return "JP";
                case "HP": return "ASIA";
                case "KP": return "KR";
                case "IV": case "IP": return "HB";
            }
            return contentId.Substring(0, 2);
        }
    }
}
