using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

namespace Sleepy
{
    /// <summary>User settings, persisted between runs.</summary>
    public class Config
    {
        public List<string> Folders { get; set; } = new List<string>();
        public string ConsoleHost { get; set; } = "";
        public int ConsolePort { get; set; } = 12800;
        public int Concurrency { get; set; } = 1;
        public string PcAddress { get; set; } = "";
        public int FilePort { get; set; } = 12790;
        public bool AutoConnect { get; set; } = true;
    }

    public class ConfigStore
    {
        readonly object _lock = new object();
        readonly string _path;
        Config _cfg = new Config();

        public static string Dir
        {
            get
            {
                var d = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "SleepyPackageInstaller");
                Directory.CreateDirectory(d);
                return d;
            }
        }

        public ConfigStore()
        {
            _path = Path.Combine(Dir, "config.json");
            try
            {
                if (File.Exists(_path))
                    _cfg = new JavaScriptSerializer().Deserialize<Config>(File.ReadAllText(_path, Encoding.UTF8)) ?? new Config();
            }
            catch { _cfg = new Config(); }
            Normalize(_cfg);
        }

        static void Normalize(Config c)
        {
            if (c.Folders == null) c.Folders = new List<string>();
            if (c.ConsolePort <= 0 || c.ConsolePort > 65535) c.ConsolePort = 12800;
            if (c.Concurrency < 1) c.Concurrency = 1;
            if (c.Concurrency > 4) c.Concurrency = 4;
            if (c.FilePort <= 0 || c.FilePort > 65535) c.FilePort = 12790;
            if (c.PcAddress == null) c.PcAddress = "";
            if (c.ConsoleHost == null) c.ConsoleHost = "";
        }

        /// <summary>A copy of the settings; safe to read from any thread.</summary>
        public Config Get()
        {
            lock (_lock)
            {
                var c = new Config
                {
                    Folders = new List<string>(_cfg.Folders), ConsoleHost = _cfg.ConsoleHost, ConsolePort = _cfg.ConsolePort,
                    Concurrency = _cfg.Concurrency, PcAddress = _cfg.PcAddress, FilePort = _cfg.FilePort, AutoConnect = _cfg.AutoConnect,
                };
                return c;
            }
        }

        public void Update(Action<Config> change)
        {
            string json;
            lock (_lock)
            {
                change(_cfg);
                Normalize(_cfg);
                json = new JavaScriptSerializer().Serialize(_cfg);
            }
            try { File.WriteAllText(_path, json, Encoding.UTF8); } catch { }
        }
    }
}
