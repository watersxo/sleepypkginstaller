using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Net.Http;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace Sleepy
{
    /// <summary>A failure reported by the console app.</summary>
    public class ConsoleException : Exception
    {
        public long Code;
        public ConsoleException(string message, long code = 0) : base(message) { Code = code; }
    }

    /// <summary>A task as reported by the console (/api/status).</summary>
    public class ConsoleTask
    {
        public int TaskId;
        public string State = "";
        public long Transferred, Total, Speed;
        public int Percent, RestSec;
        public long ErrorCode;
    }

    public class ConsoleStatus
    {
        public string ServerState = "";
        public string Message = "";
        public List<ConsoleTask> Tasks = new List<ConsoleTask>();
    }

    public class FoundConsole
    {
        public string Host;
        public int Port;
        public bool Sleepy;
        public string Label { get { return Host + ":" + Port + (Sleepy ? "   Sleepy Package Installer" : "   Package installer"); } }
        public override string ToString() { return Label; }
    }

    /// <summary>Talks to the installer app on the PS4 (synchronous; call from worker threads).</summary>
    public class ConsoleClient
    {
        static readonly Regex HexNumber = new Regex(@"(:\s*)0[xX]([0-9A-Fa-f]+)", RegexOptions.Compiled);
        readonly HttpClient _http;

        public ConsoleClient()
        {
            ServicePointManager.Expect100Continue = false;
            ServicePointManager.DefaultConnectionLimit = 64;
            var handler = new HttpClientHandler { UseProxy = false };
            _http = new HttpClient(handler) { Timeout = TimeSpan.FromMinutes(3) };
            _http.DefaultRequestHeaders.ConnectionClose = true;
        }

        // The console app prints some numbers as 0x..., which is not valid JSON.
        public static Dictionary<string, object> ParseLoose(string json)
        {
            string fixedJson = HexNumber.Replace(json, m =>
            {
                ulong n;
                return ulong.TryParse(m.Groups[2].Value, System.Globalization.NumberStyles.HexNumber, null, out n) ? m.Groups[1].Value + n : m.Value;
            });
            var ser = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
            return ser.DeserializeObject(fixedJson) as Dictionary<string, object>;
        }

        static string Url(string host, int port, string path) { return "http://" + host + ":" + port + path; }

        Dictionary<string, object> Call(string host, int port, string path, object body, TimeSpan timeout)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            bool routine = path == "/api/status" || path == "/api/get_task_progress";
            string sent = body == null ? "" : new JavaScriptSerializer().Serialize(body);
            string tag = (body == null ? "GET " : "POST ") + host + ":" + port + path;
            string text = "";
            int code = 0;
            try
            {
                using (var cts = new CancellationTokenSource(timeout))
                {
                    HttpResponseMessage resp;
                    if (body == null) resp = _http.GetAsync(Url(host, port, path), cts.Token).GetAwaiter().GetResult();
                    else
                    {
                        var content = new StringContent(sent, Encoding.UTF8, "application/json");
                        resp = _http.PostAsync(Url(host, port, path), content, cts.Token).GetAwaiter().GetResult();
                    }
                    using (resp)
                    {
                        code = (int)resp.StatusCode;
                        text = resp.Content.ReadAsStringAsync().GetAwaiter().GetResult();
                        Dictionary<string, object> map;
                        try { map = ParseLoose(text); } catch { map = null; }
                        if (map == null)
                        {
                            AppLog.Write("{0} -> HTTP {1} unreadable reply after {2} ms: {3}", tag, code, sw.ElapsedMilliseconds, Clip(text));
                            throw new ConsoleException("unexpected reply from the console (HTTP " + code + ")");
                        }
                        object st;
                        bool ok = map.TryGetValue("status", out st) && (st as string) == "success";
                        if (!routine || !ok)
                            AppLog.Write("{0} {1} -> HTTP {2} {3} in {4} ms{5}", tag, Clip(sent), code, ok ? "ok" : "FAILED " + Clip(text), sw.ElapsedMilliseconds, "");
                        return map;
                    }
                }
            }
            catch (ConsoleException) { throw; }
            catch (Exception ex)
            {
                // a console that is switched off would otherwise fill the log with one line per poll
                if (!routine || Environment.TickCount - _lastRoutineFail > 10000)
                {
                    if (routine) _lastRoutineFail = Environment.TickCount;
                    AppLog.Write("{0} {1} -> no answer after {2} ms (HTTP {3}): {4}: {5}", tag, Clip(sent), sw.ElapsedMilliseconds, code, ex.GetType().Name, ex.Message);
                }
                throw;
            }
        }

        int _lastRoutineFail = Environment.TickCount - 20000;

        static string Clip(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            s = s.Replace("\r", " ").Replace("\n", " ");
            return s.Length <= 300 ? s : s.Substring(0, 300) + "...";
        }

        /// <summary>Downloads the console app's own log file (the end of it, including the previous run).</summary>
        public string ConsoleLog(string host, int port)
        {
            using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(8)))
            {
                var resp = _http.GetAsync(Url(host, port, "/api/log"), cts.Token).GetAwaiter().GetResult();
                using (resp)
                {
                    if (!resp.IsSuccessStatusCode) throw new ConsoleException("this installer version has no log (HTTP " + (int)resp.StatusCode + "); update it to v1.10 or newer");
                    return resp.Content.ReadAsStringAsync().GetAwaiter().GetResult();
                }
            }
        }

        static void ThrowIfFailed(Dictionary<string, object> m)
        {
            object s;
            if (m.TryGetValue("status", out s) && (s as string) == "success") return;
            string msg = m.ContainsKey("error") ? m["error"] as string : null;
            long code = m.ContainsKey("error_code") ? Convert.ToInt64(m["error_code"]) : 0;
            if (string.IsNullOrEmpty(msg))
                msg = (uint)code == 0x80990088 ? "already installed" : "console error 0x" + ((uint)code).ToString("X8");
            throw new ConsoleException(msg, code);
        }

        static long L(Dictionary<string, object> m, string k) { object v; return m.TryGetValue(k, out v) && v != null ? Convert.ToInt64(v) : 0; }
        static string S(Dictionary<string, object> m, string k) { object v; return m.TryGetValue(k, out v) ? v as string ?? "" : ""; }

        /// <summary>Returns null for older installers that have no /api/status.</summary>
        public ConsoleStatus Status(string host, int port)
        {
            Dictionary<string, object> m;
            try { m = Call(host, port, "/api/status", null, TimeSpan.FromSeconds(4)); }
            catch (ConsoleException) { return null; }
            if (S(m, "status") != "success") return null;

            var st = new ConsoleStatus { ServerState = S(m, "server_state"), Message = S(m, "message") };
            var arr = m.ContainsKey("tasks") ? m["tasks"] as System.Collections.IEnumerable : null;
            if (arr != null)
                foreach (var o in arr)
                {
                    var t = o as Dictionary<string, object>;
                    if (t == null) continue;
                    st.Tasks.Add(new ConsoleTask
                    {
                        TaskId = (int)L(t, "task_id"), State = S(t, "state"), Transferred = L(t, "transferred"), Total = L(t, "total"),
                        Percent = (int)L(t, "percent"), Speed = L(t, "speed"), RestSec = (int)L(t, "rest_sec"), ErrorCode = L(t, "error_code"),
                    });
                }
            return st;
        }

        public int Install(string host, int port, string url)
        {
            var m = Call(host, port, "/api/install", new Dictionary<string, object> { { "type", "direct" }, { "packages", new[] { url } } }, TimeSpan.FromMinutes(3));
            ThrowIfFailed(m);
            long id = m.ContainsKey("task_id") ? L(m, "task_id") : -1;
            if (id < 0) throw new ConsoleException("the console did not return a task");
            return (int)id;
        }

        public void TaskOp(string host, int port, string op, int taskId)
        {
            ThrowIfFailed(Call(host, port, "/api/" + op, new Dictionary<string, object> { { "task_id", taskId } }, TimeSpan.FromSeconds(20)));
        }

        /// <summary>Fallback progress for installers without /api/status.</summary>
        public ConsoleTask Progress(string host, int port, int taskId)
        {
            var m = Call(host, port, "/api/get_task_progress", new Dictionary<string, object> { { "task_id", taskId } }, TimeSpan.FromSeconds(10));
            ThrowIfFailed(m);
            var t = new ConsoleTask { TaskId = taskId, ErrorCode = L(m, "error"), RestSec = (int)L(m, "rest_sec_total") };
            t.Total = L(m, "length_total"); t.Transferred = L(m, "transferred_total");
            if (t.Total == 0) { t.Total = L(m, "length"); t.Transferred = L(m, "transferred"); }
            if (t.Total > 0) t.Percent = (int)(t.Transferred * 100 / t.Total);
            t.State = t.ErrorCode != 0 ? "Error" : (t.Total > 0 && t.Transferred >= t.Total) ? "Complete" : t.Transferred > 0 ? "Downloading" : "Queued";
            return t;
        }

        public bool Exists(string host, int port, string titleId)
        {
            var m = Call(host, port, "/api/is_exists", new Dictionary<string, object> { { "title_id", titleId } }, TimeSpan.FromSeconds(10));
            ThrowIfFailed(m);
            object v;
            if (!m.TryGetValue("exists", out v)) return false;
            if (v is bool) return (bool)v;
            return string.Equals(v as string, "true", StringComparison.OrdinalIgnoreCase);
        }

        // ---------- discovery ----------

        static IEnumerable<IPAddress> LocalSubnetHosts()
        {
            var seen = new HashSet<string>();
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                {
                    if (ua.Address.AddressFamily != AddressFamily.InterNetwork || !IsPrivate(ua.Address)) continue;
                    var b = ua.Address.GetAddressBytes();
                    // scan the /24 around the PC's address
                    for (int i = 1; i < 255; i++)
                    {
                        var host = new IPAddress(new[] { b[0], b[1], b[2], (byte)i });
                        if (host.Equals(ua.Address)) continue;
                        if (seen.Add(host.ToString())) yield return host;
                    }
                }
            }
        }

        public static bool IsPrivate(IPAddress ip)
        {
            var b = ip.GetAddressBytes();
            if (b.Length != 4) return false;
            return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254) || b[0] == 127;
        }

        async Task<bool> TcpOpen(IPAddress ip, int port, int timeoutMs)
        {
            using (var c = new TcpClient())
            {
                var t = c.ConnectAsync(ip, port);
                if (await Task.WhenAny(t, Task.Delay(timeoutMs)).ConfigureAwait(false) != t) return false;
                return !t.IsFaulted && c.Connected;
            }
        }

        /// <summary>Looks for installer apps on the local network.</summary>
        public async Task<List<FoundConsole>> Discover()
        {
            var hosts = LocalSubnetHosts().ToList();
            var found = new List<FoundConsole>();
            // most likely ports first; the rest only if nothing answers
            foreach (var ports in new[] { new[] { 12800, 12801 }, new[] { 12802, 12803, 12804, 12805, 12806, 12807, 12808, 12809 } })
            {
                var gate = new SemaphoreSlim(250);
                var tasks = new List<Task>();
                foreach (var ip in hosts)
                    foreach (var port in ports)
                    {
                        await gate.WaitAsync().ConfigureAwait(false);
                        var h = ip; var p = port;
                        tasks.Add(Task.Run(async () =>
                        {
                            try
                            {
                                if (!await TcpOpen(h, p, 500).ConfigureAwait(false)) return;
                                var f = new FoundConsole { Host = h.ToString(), Port = p };
                                var st = Status(f.Host, p);
                                if (st != null) f.Sleepy = true;
                                else
                                {
                                    try { Exists(f.Host, p, "NONE00000"); } catch (ConsoleException) { } catch { return; }
                                }
                                lock (found) found.Add(f);
                            }
                            catch { }
                            finally { gate.Release(); }
                        }));
                    }
                await Task.WhenAll(tasks).ConfigureAwait(false);
                if (found.Count > 0) break;
            }
            return found;
        }

        /// <summary>This PC's address as seen from the console's network.</summary>
        public static string OutboundIp(string host, int port)
        {
            try
            {
                using (var s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
                {
                    s.Connect(host, port);
                    return ((IPEndPoint)s.LocalEndPoint).Address.ToString();
                }
            }
            catch { return ""; }
        }
    }
}
