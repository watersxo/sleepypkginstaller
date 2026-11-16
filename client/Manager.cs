using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Threading;
using System.Threading.Tasks;

namespace Sleepy
{
    public static class JobState
    {
        public const string Queued = "queued", Sending = "sending", Downloading = "downloading", Paused = "paused",
                            Done = "done", Error = "error", Canceled = "canceled";
    }

    /// <summary>One package being sent to the console.</summary>
    public class Job
    {
        public int Id;
        public string PkgId, Name, TitleId, Kind, Version;
        public long Size;
        public string State = JobState.Queued;
        public string Phase = "";
        public int TaskId = -1;
        public int Percent;
        public long Transferred, Total, Speed;
        public int Eta;
        public string Error = "";
        public string Detail = "";
        public string Note = "";
        public int Dep;
        public long Added, Started, Finished;
        internal int Missing;
        internal DateTime StallSince = DateTime.MinValue;
        internal bool WaitingForGame;

        public Job Clone() { return (Job)MemberwiseClone(); }
    }

    /// <summary>What the UI shows about the console connection.</summary>
    public class ConsoleState
    {
        public string Host = "";
        public int Port = 12800;
        public bool Connected;
        public bool Sleepy;
        public string Message = "";
        public string Error = "";
        public int Tasks;
    }

    /// <summary>Owns the connection to the console and the send queue.</summary>
    public class Manager
    {
        readonly ConfigStore _cfg;
        readonly Library _lib;
        readonly FileServer _fs;
        readonly ConsoleClient _con = new ConsoleClient();
        readonly object _mu = new object();

        readonly List<Job> _jobs = new List<Job>();
        int _nextId = 1;
        ConsoleState _state = new ConsoleState();
        Dictionary<string, bool> _installed = new Dictionary<string, bool>();
        int _fails;
        int _connGen;
        long _lastBytes, _rxRate;
        int _ticking;
        Timer _timer;

        public ConsoleClient Console { get { return _con; } }

        public Manager(ConfigStore cfg, Library lib, FileServer fs)
        {
            _cfg = cfg; _lib = lib; _fs = fs;
            var c = cfg.Get();
            _state.Host = c.ConsoleHost; _state.Port = c.ConsolePort;
        }

        public void Start() { _timer = new Timer(_ => Tick(), null, 1000, 1000); }
        public void Stop() { _timer?.Dispose(); }

        /// <summary>True while the app still has work that needs it to stay alive.</summary>
        public bool Busy
        {
            get
            {
                lock (_mu)
                    return _jobs.Any(j => j.State == JobState.Queued || j.State == JobState.Sending || j.State == JobState.Downloading || j.State == JobState.Paused);
            }
        }

        public void Snapshot(out ConsoleState state, out List<Job> jobs, out Dictionary<string, bool> installed, out long rx, out int serving)
        {
            lock (_mu)
            {
                state = new ConsoleState
                {
                    Host = _state.Host, Port = _state.Port, Connected = _state.Connected, Sleepy = _state.Sleepy,
                    Message = _state.Message, Error = _state.Error, Tasks = _state.Tasks,
                };
                jobs = _jobs.Select(j => j.Clone()).ToList();
                installed = new Dictionary<string, bool>(_installed);
                rx = _rxRate;
            }
            serving = _fs.Active;
        }

        public string PcAddress()
        {
            var a = _cfg.Get().PcAddress;
            if (!string.IsNullOrEmpty(a)) return a;
            string host; int port;
            lock (_mu) { host = _state.Host; port = _state.Port; }
            return string.IsNullOrEmpty(host) ? "" : ConsoleClient.OutboundIp(host, port);
        }

        // ---------- connection ----------

        /// <summary>Points the client at a console and checks that an installer answers there. Throws with a readable message.</summary>
        public void Connect(string host, int port)
        {
            host = (host ?? "").Trim();
            IPAddress ip;
            if (host.Length == 0) throw new Exception("Enter the console's IP address.");
            if (!IPAddress.TryParse(host, out ip) || ip.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
                throw new Exception("Enter a valid IPv4 address such as 192.168.1.20.");
            if (port <= 0 || port > 65535) port = 12800;

            ConsoleStatus st;
            try { st = _con.Status(host, port); }
            catch (Exception) { throw new Exception("Cannot reach " + host + ":" + port + ". Is Sleepy Package Installer open on the console?"); }

            bool sleepy = st != null;
            if (!sleepy)
            {
                try { _con.Exists(host, port, "NONE00000"); }
                catch (Exception) { throw new Exception(host + ":" + port + " does not look like a package installer."); }
            }

            _cfg.Update(c => { c.ConsoleHost = host; c.ConsolePort = port; });
            lock (_mu)
            {
                _connGen++;
                _fails = 0;
                _state = new ConsoleState { Host = host, Port = port, Connected = true, Sleepy = sleepy, Message = st?.ServerState ?? "", Tasks = st?.Tasks.Count ?? 0 };
                _installed = new Dictionary<string, bool>();
            }
            Task.Run(() => RefreshInstalled());
        }

        public void Disconnect()
        {
            lock (_mu)
            {
                _connGen++;
                _state.Connected = false; _state.Error = ""; _state.Message = "";
                _installed = new Dictionary<string, bool>();
            }
        }

        bool Target(out string host, out int port)
        {
            lock (_mu) { host = _state.Host; port = _state.Port; return _state.Connected; }
        }

        /// <summary>Asks the console which of the library's games are installed.</summary>
        public void RefreshInstalled()
        {
            string host; int port;
            if (!Target(out host, out port)) return;
            int gen; lock (_mu) gen = _connGen;

            foreach (var g in _lib.Groups.ToList())
            {
                if (g.Base == null) continue;
                lock (_mu) if (_connGen != gen) return;
                try
                {
                    bool ex = _con.Exists(host, port, g.TitleId);
                    lock (_mu) if (_connGen == gen) _installed[g.TitleId] = ex;
                }
                catch { }
            }
        }

        // ---------- main loop ----------

        void Tick()
        {
            if (Interlocked.Exchange(ref _ticking, 1) == 1) return;
            try
            {
                long cur = _fs.Bytes;
                lock (_mu) { _rxRate = cur - _lastBytes; _lastBytes = cur; }

                string host; int port;
                bool connected = Target(out host, out port);
                if (string.IsNullOrEmpty(host)) return;

                if (!connected)
                {
                    bool lost; lock (_mu) lost = _connGen > 0 && _state.Error.Length > 0;
                    if (lost && _cfg.Get().AutoConnect)
                    {
                        try
                        {
                            var st = _con.Status(host, port);
                            if (st != null) lock (_mu) { _state.Connected = true; _state.Sleepy = true; _state.Error = ""; _fails = 0; _state.Message = st.ServerState; }
                        }
                        catch { }
                    }
                    return;
                }

                Poll(host, port);
                Schedule();
            }
            catch { }
            finally { Volatile.Write(ref _ticking, 0); }
        }

        void MarkFailure()
        {
            lock (_mu)
            {
                if (++_fails >= 3) { _state.Connected = false; _state.Error = "The connection to the console was lost."; }
            }
        }

        // Refreshes the progress of every job that is on the console.
        void Poll(string host, int port)
        {
            List<Job> active;
            bool sleepy;
            lock (_mu)
            {
                active = _jobs.Where(j => (j.State == JobState.Downloading || j.State == JobState.Paused) && j.TaskId >= 0).ToList();
                sleepy = _state.Sleepy;
            }

            if (sleepy)
            {
                ConsoleStatus st;
                try { st = _con.Status(host, port); }
                catch { MarkFailure(); return; }
                if (st == null) { MarkFailure(); return; }

                lock (_mu)
                {
                    _fails = 0;
                    _state.Message = st.ServerState;
                    _state.Tasks = st.Tasks.Count;
                    var byId = st.Tasks.GroupBy(t => t.TaskId).ToDictionary(g => g.Key, g => g.Last());
                    foreach (var j in active)
                    {
                        ConsoleTask t;
                        if (byId.TryGetValue(j.TaskId, out t)) { j.Missing = 0; Apply(j, t); }
                        else if (++j.Missing >= 3) Fail(j, "The task is no longer on the console (was the installer restarted?).");
                    }
                }
                return;
            }

            foreach (var j in active)
            {
                ConsoleTask t;
                try { t = _con.Progress(host, port, j.TaskId); }
                catch (ConsoleException) { continue; }
                catch { MarkFailure(); return; }
                lock (_mu) { _fails = 0; Apply(j, t); }
            }
        }

        static long Now() { return DateTimeOffset.UtcNow.ToUnixTimeSeconds(); }

        // Marks a job failed and records everything useful for a bug report.
        void Fail(Job j, string msg)
        {
            j.State = JobState.Error; j.Error = msg; j.Speed = 0; j.Finished = Now();
            j.Detail = BuildDetail(j, msg);
        }

        string BuildDetail(Job j, string msg)
        {
            var sb = new System.Text.StringBuilder();
            sb.AppendLine("Sleepy Package Installer client " + App.ClientVersion);
            sb.AppendLine("Time: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
            sb.AppendLine("Error: " + msg);
            sb.AppendLine("Job: " + j.Name + " (" + j.Kind + (string.IsNullOrEmpty(j.Version) ? "" : " " + j.Version) + "), state before failure: " + j.Phase);
            var p = _lib.Pkg(j.PkgId);
            if (p != null)
            {
                sb.AppendLine("File: " + p.Path);
                sb.AppendLine("File size: " + p.Size + " bytes; size in package header: " + p.PkgSize + (p.Incomplete ? " (MISMATCH: incomplete file)" : ""));
                sb.AppendLine("Content ID: " + p.ContentId + "; title ID: " + p.TitleId + "; category: " + p.Category + "; app version: " + p.Version);
                sb.AppendLine("Served as: " + _fs.Url(PcAddressFast(), p.Id));
            }
            ConsoleState s; lock (_mu) { s = _state; }
            sb.AppendLine("Console: " + s.Host + ":" + s.Port + (s.Sleepy ? " (Sleepy Package Installer)" : " (older installer)") + ", state: " + s.Message);
            sb.AppendLine("Console task id: " + j.TaskId);
            return sb.ToString();
        }

        string PcAddressFast()
        {
            var a = _cfg.Get().PcAddress;
            return string.IsNullOrEmpty(a) ? "<this PC>" : a;
        }

        void Apply(Job j, ConsoleTask t)
        {
            if (t.Total > 0) j.Total = t.Total; else if (j.Total == 0) j.Total = j.Size;
            j.Transferred = t.Transferred; j.Speed = t.Speed; j.Eta = t.RestSec; j.Phase = t.State;
            if (j.Total > 0) j.Percent = (int)Math.Min(100, t.Transferred * 100 / j.Total);

            // The console reports "Waiting" when no bytes have moved for a few seconds. At the end of a download
            // (updates stop at about 98-99%) this means it is finished and only waiting for the system to install it.
            if (t.State == "Waiting" && j.Percent >= 97)
            {
                if (j.StallSince == DateTime.MinValue) j.StallSince = DateTime.UtcNow;
                if ((DateTime.UtcNow - j.StallSince).TotalSeconds >= 10)
                {
                    j.State = JobState.Done; j.Percent = 100; j.Speed = 0; j.Eta = 0; j.Finished = Now(); j.Phase = "Waiting to install";
                    if (j.Kind == Kinds.Update)
                        j.Note = "Downloaded. On the PS4, close Sleepy Package Installer, then open Notifications > Downloads and press Install.";
                    return;
                }
            }
            else j.StallSince = DateTime.MinValue;

            switch (t.State)
            {
                case "Complete":
                    j.State = JobState.Done; j.Percent = 100; j.Speed = 0; j.Eta = 0; j.Finished = Now(); j.Phase = "Complete";
                    break;
                case "Error":
                    Fail(j, DescribeError(t.ErrorCode));
                    break;
                case "Paused":
                case "Stopped":
                    j.State = JobState.Paused; j.Speed = 0;
                    break;
                default:
                    j.State = JobState.Downloading;
                    break;
            }
        }

        static string DescribeError(long code)
        {
            switch ((uint)code)
            {
                case 0x80990088: return "Already installed.";
                case 0x80990015:
                case 0x80990016: return "Not enough free space on the console.";
                case 0x80990004: return "Install error 0x80990004.";
                case 0: return "The download failed.";
            }
            return "The download failed (error 0x" + ((uint)code).ToString("X8") + ").";
        }

        // Starts queued jobs while there is capacity.
        void Schedule()
        {
            int limit = _cfg.Get().Concurrency;
            var start = new List<Job>();
            lock (_mu)
            {
                int active = _jobs.Count(j => (j.State == JobState.Sending && !j.WaitingForGame) || j.State == JobState.Downloading || j.State == JobState.Paused);
                foreach (var j in _jobs)
                {
                    if (active >= limit) break;
                    if (j.State != JobState.Queued) continue;
                    if (j.Dep != 0)
                    {
                        var dep = _jobs.FirstOrDefault(x => x.Id == j.Dep);
                        if (dep != null && (dep.State == JobState.Error || dep.State == JobState.Canceled)) { Fail(j, "Skipped because the game failed to install."); continue; }
                        if (dep != null && dep.State != JobState.Done) continue; // wait for the game
                    }
                    j.State = JobState.Sending; j.Phase = "Contacting console"; j.Error = ""; j.Started = Now();
                    start.Add(j);
                    active++;
                }
            }
            foreach (var j in start) { var job = j; Task.Run(() => Send(job)); }
        }

        void SetPhase(Job j, string phase) { lock (_mu) if (j.State == JobState.Sending) j.Phase = phase; }

        void FailJob(Job j, string msg)
        {
            if (msg.Contains("Unable to download package header") || msg.Contains("Unable to set up prerequisites"))
                msg += " The console could not reach this PC: allow Sleepy Package Installer through Windows Firewall (private networks).";
            lock (_mu) Fail(j, msg);
        }

        // Registers a package on the console and starts the download.
        void Send(Job j)
        {
            string host; int port;
            if (!Target(out host, out port)) { lock (_mu) { j.State = JobState.Queued; j.Phase = ""; } return; }

            var pkg = _lib.Pkg(j.PkgId);
            if (pkg == null) { FailJob(j, "The package is no longer in the library."); return; }
            try
            {
                if (new FileInfo(pkg.Path).Length != pkg.Size) { FailJob(j, "The package file changed. Rescan the library."); return; }
            }
            catch { FailJob(j, "The package file was removed. Rescan the library."); return; }
            if (pkg.Incomplete) { FailJob(j, "The package is incomplete (smaller than its header says)."); return; }

            // updates and add-ons need the game to be installed first; while they wait, other downloads may start
            if (j.Dep != 0)
            {
                SetPhase(j, "Waiting for the game to finish installing");
                lock (_mu) j.WaitingForGame = true;
                var deadline = DateTime.UtcNow.AddMinutes(45);
                while (DateTime.UtcNow < deadline)
                {
                    lock (_mu) if (j.State != JobState.Sending) { j.WaitingForGame = false; return; }
                    try { if (_con.Exists(host, port, j.TitleId)) break; } catch { }
                    Thread.Sleep(2000);
                }
                lock (_mu) { j.WaitingForGame = false; if (j.State != JobState.Sending) return; }
            }

            string pc = _cfg.Get().PcAddress;
            if (string.IsNullOrEmpty(pc)) pc = ConsoleClient.OutboundIp(host, port);
            if (string.IsNullOrEmpty(pc)) { FailJob(j, "Cannot determine this PC's address on the console's network. Set it in Settings."); return; }

            SetPhase(j, "Registering on console");
            int taskId;
            try { taskId = _con.Install(host, port, _fs.Url(pc, j.PkgId)); }
            catch (ConsoleException ex) { FailJob(j, ex.Message); return; }
            catch (Exception)
            {
                MarkFailure();
                lock (_mu) { j.State = JobState.Queued; j.Phase = ""; } // retry once the console is back
                return;
            }

            bool canceled;
            lock (_mu) { canceled = j.State == JobState.Canceled; j.TaskId = taskId; }
            if (canceled) { try { _con.TaskOp(host, port, "unregister_task", taskId); } catch { } return; }

            SetPhase(j, "Starting download");
            try { _con.TaskOp(host, port, "start_task", taskId); }
            catch (Exception ex) { FailJob(j, "Could not start the download: " + ex.Message); return; }

            lock (_mu)
            {
                if (j.State == JobState.Sending) { j.State = JobState.Downloading; j.Phase = "Downloading"; j.Total = j.Size; j.Missing = 0; }
            }
        }

        // ---------- user actions ----------

        /// <summary>Adds packages to the queue in the given order; returns how many were added.</summary>
        public int Enqueue(IEnumerable<string> pkgIds)
        {
            string h; int p;
            if (!Target(out h, out p)) throw new Exception("Connect to the console first.");

            int added = 0;
            lock (_mu)
            {
                var busy = new HashSet<string>(_jobs.Where(j => j.State == JobState.Queued || j.State == JobState.Sending
                                                              || j.State == JobState.Downloading || j.State == JobState.Paused).Select(j => j.PkgId));
                var baseJob = new Dictionary<string, int>();
                foreach (var id in pkgIds)
                {
                    var pkg = _lib.Pkg(id);
                    if (pkg == null || busy.Contains(id)) continue;
                    busy.Add(id);

                    var j = new Job
                    {
                        Id = _nextId++, PkgId = id, Name = pkg.Title, TitleId = pkg.TitleId, Kind = pkg.Kind, Version = pkg.Version,
                        Size = pkg.Size, Total = pkg.Size, Added = Now(),
                    };
                    int depId;
                    if (pkg.Kind == Kinds.Base) baseJob[pkg.TitleId] = j.Id;
                    else if (baseJob.TryGetValue(pkg.TitleId, out depId)) j.Dep = depId;
                    _jobs.Add(j);
                    added++;
                }
            }
            return added;
        }

        public void JobOp(int id, string op)
        {
            string host; int port;
            Target(out host, out port);

            Job j; string state; int task;
            lock (_mu)
            {
                j = _jobs.FirstOrDefault(x => x.Id == id);
                if (j == null) return;
                state = j.State; task = j.TaskId;
            }

            switch (op)
            {
                case "pause":
                    if (state != JobState.Downloading) return;
                    _con.TaskOp(host, port, "pause_task", task);
                    lock (_mu) { j.State = JobState.Paused; j.Phase = "Paused"; j.Speed = 0; }
                    break;
                case "resume":
                    if (state != JobState.Paused) return;
                    _con.TaskOp(host, port, "resume_task", task);
                    lock (_mu) { j.State = JobState.Downloading; j.Phase = "Downloading"; }
                    break;
                case "cancel":
                    if (state == JobState.Queued || state == JobState.Sending)
                        lock (_mu) { j.State = JobState.Canceled; j.Phase = "Canceled"; j.Finished = Now(); }
                    else if (state == JobState.Downloading || state == JobState.Paused)
                    {
                        try { _con.TaskOp(host, port, "stop_task", task); } catch { }
                        try { _con.TaskOp(host, port, "unregister_task", task); } catch { }
                        lock (_mu) { j.State = JobState.Canceled; j.Phase = "Canceled"; j.Speed = 0; j.Finished = Now(); }
                    }
                    break;
                case "retry":
                    if (state != JobState.Error && state != JobState.Canceled) return;
                    lock (_mu)
                    {
                        j.State = JobState.Queued; j.Phase = ""; j.Error = ""; j.Detail = ""; j.Note = ""; j.TaskId = -1; j.Percent = 0; j.Transferred = 0;
                        j.Total = j.Size; j.Speed = 0; j.Eta = 0; j.Missing = 0; j.Added = Now();
                    }
                    break;
                case "remove":
                    if (state == JobState.Downloading || state == JobState.Paused || state == JobState.Sending) throw new Exception("Cancel the job first.");
                    lock (_mu) _jobs.Remove(j);
                    break;
            }
        }

        public void ClearFinished()
        {
            lock (_mu) _jobs.RemoveAll(j => j.State == JobState.Done || j.State == JobState.Error || j.State == JobState.Canceled);
        }

        public void BulkOp(string op)
        {
            List<int> ids;
            lock (_mu)
                ids = _jobs.Where(j => (op == "pause" && j.State == JobState.Downloading) || (op == "resume" && j.State == JobState.Paused)).Select(j => j.Id).ToList();
            foreach (var id in ids) { try { JobOp(id, op); } catch { } }
        }
    }
}
