using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Sleepy
{
    public static class Kinds
    {
        public const string Base = "base";
        public const string Update = "update";
        public const string Dlc = "dlc";
        public const string Other = "other";
    }

    /// <summary>Everything the client needs to know about one .pkg file.</summary>
    public class PkgInfo
    {
        public string Id { get; set; }
        public string Path { get; set; }
        public long Size { get; set; }
        public long ModTime { get; set; }
        public string ContentId { get; set; }
        public string TitleId { get; set; }
        public string Title { get; set; }
        public string Category { get; set; }  // param.sfo category: gd, gp, ac ...
        public string Kind { get; set; }      // base, update, dlc, other
        public string Version { get; set; }   // APP_VER
        public long PkgSize { get; set; }     // size declared by the package header
        public bool Incomplete { get; set; }
        public bool HasIcon { get; set; }
    }

    public static class PkgReader
    {
        const int HeaderSize = 0x2000;
        const uint EntryParamSfo = 0x1000;
        const uint EntryIcon0 = 0x1200;
        const int MaxSfoSize = 256 * 1024;
        const int MaxIconSize = 2 * 1024 * 1024;

        static uint Be32(byte[] b, int o) { return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]); }
        static ulong Be64(byte[] b, int o) { return ((ulong)Be32(b, o) << 32) | Be32(b, o + 4); }
        static string CStr(byte[] b, int o, int max)
        {
            int e = o;
            while (e < o + max && e < b.Length && b[e] != 0) e++;
            return Encoding.UTF8.GetString(b, o, e - o);
        }

        /// <summary>Reads the header, param.sfo and optionally icon0.png. Only a few KB at the start of the file are read.</summary>
        public static PkgInfo Read(string path, bool wantIcon, out byte[] icon)
        {
            icon = null;
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 4096, FileOptions.RandomAccess))
            {
                var hdr = new byte[HeaderSize];
                if (!ReadAt(fs, hdr, 0) || hdr[0] != 0x7F || hdr[1] != 'C' || hdr[2] != 'N' || hdr[3] != 'T')
                    throw new InvalidDataException("not a PS4 package");

                var info = new PkgInfo
                {
                    Path = path,
                    Size = fs.Length,
                    ModTime = File.GetLastWriteTimeUtc(path).Ticks,
                    ContentId = CStr(hdr, 0x40, 0x24),
                    PkgSize = (long)Be64(hdr, 0x430),
                };
                info.Incomplete = info.PkgSize > 0 && info.PkgSize != info.Size;

                uint entryCount = Be32(hdr, 0x10);
                uint entryOffset = Be32(hdr, 0x18);
                if (entryCount == 0 || entryCount > 0x10000) throw new InvalidDataException("bad entry count");

                var table = new byte[entryCount * 0x20];
                if (!ReadAt(fs, table, entryOffset)) throw new InvalidDataException("entry table");

                uint sfoOff = 0, sfoSize = 0, iconOff = 0, iconSize = 0;
                for (int i = 0; i < entryCount; i++)
                {
                    int o = i * 0x20;
                    uint id = Be32(table, o);
                    if (id == EntryParamSfo) { sfoOff = Be32(table, o + 0x10); sfoSize = Be32(table, o + 0x14); }
                    else if (id == EntryIcon0) { iconOff = Be32(table, o + 0x10); iconSize = Be32(table, o + 0x14); }
                }
                if (sfoOff == 0 || sfoSize == 0 || sfoSize > MaxSfoSize) throw new InvalidDataException("no param.sfo");

                var sfoData = new byte[sfoSize];
                if (!ReadAt(fs, sfoData, sfoOff)) throw new InvalidDataException("param.sfo");
                var sfo = ParseSfo(sfoData);

                sfo.TryGetValue("TITLE_ID", out var titleId);
                sfo.TryGetValue("TITLE", out var title);
                sfo.TryGetValue("CATEGORY", out var category);
                sfo.TryGetValue("APP_VER", out var version);
                if (sfo.TryGetValue("CONTENT_ID", out var cid) && !string.IsNullOrEmpty(cid)) info.ContentId = cid;

                info.TitleId = titleId ?? "";
                if (info.TitleId.Length == 0 && info.ContentId.Length >= 16) info.TitleId = info.ContentId.Substring(7, 9);
                info.Title = string.IsNullOrEmpty(title) ? info.TitleId : title;
                info.Category = (category ?? "").ToLowerInvariant();
                info.Version = version ?? "";
                info.Kind = KindOf(info.Category);

                if (iconOff > 0 && iconSize > 0 && iconSize <= MaxIconSize)
                {
                    info.HasIcon = true;
                    if (wantIcon)
                    {
                        var data = new byte[iconSize];
                        if (ReadAt(fs, data, iconOff)) icon = data; else info.HasIcon = false;
                    }
                }
                return info;
            }
        }

        static bool ReadAt(FileStream fs, byte[] buf, long offset)
        {
            if (offset < 0 || offset + buf.Length > fs.Length) return false;
            fs.Position = offset;
            int got = 0;
            while (got < buf.Length)
            {
                int n = fs.Read(buf, got, buf.Length - got);
                if (n <= 0) return false;
                got += n;
            }
            return true;
        }

        public static string KindOf(string category)
        {
            if (category == "gp") return Kinds.Update;
            if (category.StartsWith("ac")) return Kinds.Dlc;
            if (category.StartsWith("gd") || category.StartsWith("bd")) return Kinds.Base;
            return Kinds.Other;
        }

        public static Dictionary<string, string> ParseSfo(byte[] d)
        {
            if (d.Length < 0x14 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') throw new InvalidDataException("invalid param.sfo");
            int keyTable = (int)BitConverter.ToUInt32(d, 8);
            int valTable = (int)BitConverter.ToUInt32(d, 0xC);
            int count = (int)BitConverter.ToUInt32(d, 0x10);
            if (count < 0 || count > 1024 || 0x14 + count * 16 > d.Length) throw new InvalidDataException("invalid param.sfo entry count");

            var res = new Dictionary<string, string>();
            for (int i = 0; i < count; i++)
            {
                int e = 0x14 + i * 16;
                int keyOff = keyTable + BitConverter.ToUInt16(d, e);
                int format = BitConverter.ToUInt16(d, e + 2);
                int size = (int)BitConverter.ToUInt32(d, e + 4);
                int valOff = valTable + (int)BitConverter.ToUInt32(d, e + 12);
                if (keyOff < 0 || keyOff >= d.Length || size < 0 || valOff < 0 || valOff + size > d.Length) continue;
                string key = CStr(d, keyOff, d.Length - keyOff);
                if (format == 0x0004 || format == 0x0204) res[key] = CStr(d, valOff, size);
                else if (format == 0x0404 && size >= 4) res[key] = BitConverter.ToUInt32(d, valOff).ToString();
            }
            return res;
        }

        /// <summary>Compares dotted versions such as "01.05" and "1.10".</summary>
        public static int CompareVersions(string a, string b)
        {
            var pa = (a ?? "").Split('.');
            var pb = (b ?? "").Split('.');
            for (int i = 0; i < Math.Max(pa.Length, pb.Length); i++)
            {
                int x = 0, y = 0;
                if (i < pa.Length) int.TryParse(pa[i], out x);
                if (i < pb.Length) int.TryParse(pb[i], out y);
                if (x != y) return x < y ? -1 : 1;
            }
            return 0;
        }
    }
}
