using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Sleepy
{
    /// <summary>
    /// Serves library packages to the console over HTTP with Range support, large buffers and keep-alive,
    /// so a single transfer can fill a gigabit link.
    /// </summary>
    public class FileServer
    {
        readonly Library _lib;
        TcpListener _listener;
        long _bytes;
        int _active;
        volatile bool _closed;

        public int Port { get; private set; }
        public long Bytes { get { return Interlocked.Read(ref _bytes); } }
        public int Active { get { return Volatile.Read(ref _active); } }
        long _requests;

        public FileServer(Library lib) { _lib = lib; }

        public string Url(string host, string id) { return "http://" + host + ":" + Port + "/p/" + id + ".pkg"; }

        /// <summary>Listens on the preferred port, or the next free one.</summary>
        public void Start(int preferred)
        {
            Exception last = null;
            for (int p = preferred; p < preferred + 20; p++)
            {
                try
                {
                    var l = new TcpListener(IPAddress.Any, p);
                    l.Server.ExclusiveAddressUse = true;
                    l.Start(128);
                    _listener = l;
                    Port = p;
                    Task.Run(AcceptLoop);
                    return;
                }
                catch (Exception ex) { last = ex; }
            }
            throw new IOException("cannot start the file server: " + (last != null ? last.Message : ""));
        }

        public void Stop()
        {
            _closed = true;
            try { _listener?.Stop(); } catch { }
        }

        async Task AcceptLoop()
        {
            while (!_closed)
            {
                TcpClient c;
                try { c = await _listener.AcceptTcpClientAsync().ConfigureAwait(false); }
                catch { if (_closed) return; continue; }
                var _ = Task.Run(() => Serve(c));
            }
        }

        async Task Serve(TcpClient client)
        {
            using (client)
            {
                try
                {
                    var remote = ((IPEndPoint)client.Client.RemoteEndPoint).Address;
                    if (!ConsoleClient.IsPrivate(remote) && !IPAddress.IsLoopback(remote)) return; // LAN devices only

                    client.NoDelay = true;
                    client.SendBufferSize = 4 << 20;
                    client.ReceiveBufferSize = 64 << 10;
                    var ns = client.GetStream();
                    var buf = new byte[1 << 20];

                    while (!_closed)
                    {
                        string head = await ReadHead(ns).ConfigureAwait(false);
                        if (head == null) return;
                        if (!await Respond(ns, head, buf).ConfigureAwait(false)) return;
                    }
                }
                catch (Exception ex)
                {
                    if (!_closed) AppLog.Write("file server: connection ended with {0}: {1}", ex.GetType().Name, ex.Message);
                }
            }
        }

        // Reads the request line and headers; returns null on disconnect, idle timeout or oversize.
        static async Task<string> ReadHead(NetworkStream ns)
        {
            var sb = new StringBuilder();
            var one = new byte[4096];
            using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(60)))
            {
                while (sb.Length < 16384)
                {
                    int n;
                    try { n = await ns.ReadAsync(one, 0, one.Length, cts.Token).ConfigureAwait(false); }
                    catch { return null; }
                    if (n <= 0) return null;
                    sb.Append(Encoding.ASCII.GetString(one, 0, n));
                    int end = sb.ToString().IndexOf("\r\n\r\n", StringComparison.Ordinal);
                    if (end >= 0) return sb.ToString(0, end);
                }
            }
            return null;
        }

        static string Header(string head, string name)
        {
            foreach (var line in head.Split(new[] { "\r\n" }, StringSplitOptions.None))
            {
                int i = line.IndexOf(':');
                if (i > 0 && string.Equals(line.Substring(0, i).Trim(), name, StringComparison.OrdinalIgnoreCase))
                    return line.Substring(i + 1).Trim();
            }
            return null;
        }

        static async Task Reply(NetworkStream ns, int code, string text, string extra = "", string body = "")
        {
            var data = Encoding.ASCII.GetBytes("HTTP/1.1 " + code + " " + text + "\r\nContent-Length: " + body.Length + "\r\nConnection: close\r\n" + extra + "\r\n" + body);
            await ns.WriteAsync(data, 0, data.Length).ConfigureAwait(false);
        }

        // Returns true to keep the connection open for another request.
        async Task<bool> Respond(NetworkStream ns, string head, byte[] buf)
        {
            var first = head.Split('\n')[0].Trim().Split(' ');
            if (first.Length < 3) { await Reply(ns, 400, "Bad Request").ConfigureAwait(false); return false; }
            string method = first[0].ToUpperInvariant(), target = first[1];
            bool keepAlive = !string.Equals(Header(head, "Connection"), "close", StringComparison.OrdinalIgnoreCase) && first[2] == "HTTP/1.1";

            if (method != "GET" && method != "HEAD") { await Reply(ns, 405, "Method Not Allowed").ConfigureAwait(false); return false; }
            int q = target.IndexOf('?');
            if (q >= 0) target = target.Substring(0, q);
            if (!target.StartsWith("/p/") || !target.EndsWith(".pkg")) { await Reply(ns, 404, "Not Found").ConfigureAwait(false); return false; }

            var pkg = _lib.Pkg(target.Substring(3, target.Length - 7));
            FileStream fs = null;
            try
            {
                if (pkg != null) fs = new FileStream(pkg.Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1 << 20, FileOptions.SequentialScan | FileOptions.Asynchronous);
            }
            catch { }
            if (fs == null) { await Reply(ns, 404, "Not Found").ConfigureAwait(false); return false; }

            using (fs)
            {
                long total = fs.Length, start = 0, end = total - 1;
                bool partial = false;
                string range = Header(head, "Range");
                if (!string.IsNullOrEmpty(range) && range.StartsWith("bytes=", StringComparison.OrdinalIgnoreCase))
                {
                    var spec = range.Substring(6).Split(',')[0].Trim();
                    int dash = spec.IndexOf('-');
                    long a, b;
                    bool okA = long.TryParse(spec.Substring(0, Math.Max(dash, 0)), out a);
                    bool okB = long.TryParse(spec.Substring(dash + 1), out b);
                    if (dash >= 0 && (okA || okB))
                    {
                        if (!okA) { start = Math.Max(0, total - b); end = total - 1; }
                        else { start = a; end = okB ? Math.Min(b, total - 1) : total - 1; }
                        if (start > end || start >= total)
                        {
                            await Reply(ns, 416, "Range Not Satisfiable", "Content-Range: bytes */" + total + "\r\n").ConfigureAwait(false);
                            return false;
                        }
                        partial = true;
                    }
                }

                long length = end - start + 1;
                if (Interlocked.Increment(ref _requests) % 50 == 1)
                    AppLog.Write("file server: {0} {1} bytes {2}-{3} of {4} (request #{5})", method, pkg.Title, start, end, total, Volatile.Read(ref _requests));
                var sb = new StringBuilder();
                sb.Append(partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n");
                sb.Append("Content-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\n");
                sb.Append("Content-Length: ").Append(length).Append("\r\n");
                if (partial) sb.Append("Content-Range: bytes ").Append(start).Append('-').Append(end).Append('/').Append(total).Append("\r\n");
                sb.Append(keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n");
                var hb = Encoding.ASCII.GetBytes(sb.ToString());
                await ns.WriteAsync(hb, 0, hb.Length).ConfigureAwait(false);

                if (method == "HEAD") return keepAlive;

                Interlocked.Increment(ref _active);
                try
                {
                    fs.Position = start;
                    long left = length;
                    while (left > 0)
                    {
                        int n = await fs.ReadAsync(buf, 0, (int)Math.Min(buf.Length, left)).ConfigureAwait(false);
                        if (n <= 0) return false;
                        await ns.WriteAsync(buf, 0, n).ConfigureAwait(false);
                        Interlocked.Add(ref _bytes, n);
                        left -= n;
                    }
                }
                finally { Interlocked.Decrement(ref _active); }
            }
            return keepAlive;
        }
    }
}
