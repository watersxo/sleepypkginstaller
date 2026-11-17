using System;
using System.IO;
using System.Text;

namespace Sleepy
{
    /// <summary>Detailed log of everything the client does, written line by line so it survives a crash.</summary>
    public static class AppLog
    {
        static readonly object Mu = new object();
        const long MaxBytes = 4 * 1024 * 1024;

        public static string Dir { get { return ConfigStore.Dir; } }
        public static string FilePath { get { return Path.Combine(Dir, "client.log"); } }
        public static string ConsoleLogPath { get { return Path.Combine(Dir, "console.log"); } }

        public static void Write(string line)
        {
            try
            {
                lock (Mu)
                {
                    var path = FilePath;
                    var fi = new FileInfo(path);
                    if (fi.Exists && fi.Length > MaxBytes)
                    {
                        var old = path + ".old";
                        if (File.Exists(old)) File.Delete(old);
                        File.Move(path, old);
                    }
                    File.AppendAllText(path, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + "  " + line + "\r\n", Encoding.UTF8);
                }
            }
            catch { }
        }

        public static void Write(string format, params object[] args)
        {
            string text;
            try { text = string.Format(format, args); } catch { text = format; }
            Write(text);
        }

        public static void Error(string what, Exception ex)
        {
            Write(what + ": " + (ex == null ? "" : ex.GetType().Name + ": " + ex.Message));
        }

        /// <summary>Keeps the console's own log (fetched over the network) next to the client log.</summary>
        public static void SaveConsoleLog(string text, string reason)
        {
            try
            {
                lock (Mu)
                    File.WriteAllText(ConsoleLogPath, "Fetched " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " (" + reason + ")\r\n\r\n" + text.Replace("\r\n", "\n").Replace("\n", "\r\n"), Encoding.UTF8);
            }
            catch { }
        }
    }
}
