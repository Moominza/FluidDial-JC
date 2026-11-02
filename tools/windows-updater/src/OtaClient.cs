// FluidDial Updater - espota protocol client.
// Reuses the stock Arduino ESP32 OTA ("espota") exchange; nothing here is a new protocol:
//   1. UDP invitation  "0 <local tcp port> <size> <md5>\n"  to <pendant>:3232
//   2. pendant answers "AUTH <nonce>"
//   3. client sends    "200 <cnonce> <md5(md5(password):nonce:cnonce)>\n"; pendant answers "OK"
//   4. pendant connects back to our TCP port and the image is sent in 1024-byte chunks, each acknowledged
//   5. pendant sends "OK" when the image is complete.
// The password is never logged, never put in a message and never written anywhere.

using System;
using System.Collections.Generic;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace FluidDialUpdater
{
    public enum OtaResult
    {
        Success,
        NoAnswer,        // no reply to the invitation: not armed / wrong address / unreachable
        AuthFailed,
        BadAnswer,
        DeviceNoConnect, // authenticated, but the pendant never connected back (firewall / refused image)
        UploadError,
        Cancelled,
        BadArgument
    }

    public class OtaOutcome
    {
        public OtaResult Result;
        public string Message;
        public bool Ok { get { return Result == OtaResult.Success; } }
        public static OtaOutcome Make(OtaResult r, string m) { OtaOutcome o = new OtaOutcome(); o.Result = r; o.Message = m; return o; }
    }

    public class CancelFlag { public volatile bool Requested; }

    public static class OtaClient
    {
        public const int DefaultPort = 3232;
        public const int ChunkSize = 1024;

        public static string Md5Hex(byte[] data)
        {
            using (MD5 md5 = MD5.Create())
            {
                return Hex(md5.ComputeHash(data));
            }
        }

        public static string Md5HexText(string text)
        {
            return Md5Hex(Encoding.UTF8.GetBytes(text));
        }

        public static string Hex(byte[] b)
        {
            StringBuilder sb = new StringBuilder(b.Length * 2);
            for (int i = 0; i < b.Length; i++) sb.Append(b[i].ToString("x2"));
            return sb.ToString();
        }

        /// <summary>The value the pendant expects: md5( md5(password) ":" nonce ":" cnonce ).</summary>
        public static string AuthResponse(string password, string nonce, string cnonce)
        {
            return Md5HexText(Md5HexText(password) + ":" + nonce + ":" + cnonce);
        }

        static IPAddress ParseAddress(string host)
        {
            IPAddress ip;
            if (IPAddress.TryParse(host, out ip) && ip.AddressFamily == AddressFamily.InterNetwork) return ip;
            try
            {
                foreach (IPAddress a in Dns.GetHostAddresses(host))
                    if (a.AddressFamily == AddressFamily.InterNetwork) return a;
            }
            catch (Exception) { }
            return null;
        }

        public static bool IsValidAddress(string host)
        {
            return host != null && host.Trim().Length > 0 && ParseAddress(host.Trim()) != null;
        }

        /// <summary>Resolve fluiddial-jc.local (the pendant advertises it only while OTA is armed).</summary>
        public static string Discover(string hostName)
        {
            try
            {
                foreach (IPAddress a in Dns.GetHostAddresses(hostName))
                    if (a.AddressFamily == AddressFamily.InterNetwork) return a.ToString();
            }
            catch (Exception) { }
            return null;
        }

        public static string PingText(string host)
        {
            try
            {
                using (Ping p = new Ping())
                {
                    PingReply r = p.Send(host, 1500);
                    if (r.Status == IPStatus.Success) return "reply in " + r.RoundtripTime + " ms";
                    return r.Status.ToString();
                }
            }
            catch (Exception ex) { return "failed (" + ex.Message + ")"; }
        }

        public static bool PingOnce(IPAddress ip, int timeoutMs)
        {
            try
            {
                using (Ping p = new Ping())
                {
                    return p.Send(ip, timeoutMs).Status == IPStatus.Success;
                }
            }
            catch (Exception) { return false; }
        }

        /// <summary>
        /// Send invitations until the pendant answers. Returns the reply ("AUTH nnn" or "OK") or null.
        /// A pendant left waiting for authentication drops the next invitation, so several attempts are normal.
        /// </summary>
        static string Invite(UdpClient udp, IPEndPoint remote, string message, int attempts, int timeoutMs,
                             CancelFlag cancel, Action<string> status)
        {
            byte[] data = Encoding.ASCII.GetBytes(message);
            udp.Client.ReceiveTimeout = timeoutMs;
            for (int i = 1; i <= attempts; i++)
            {
                if (cancel != null && cancel.Requested) return null;
                if (status != null) status("Sending invitation " + i + " of " + attempts + "...");
                try { udp.Send(data, data.Length, remote); }
                catch (SocketException) { return null; }
                try
                {
                    IPEndPoint from = new IPEndPoint(IPAddress.Any, 0);
                    byte[] r = udp.Receive(ref from);
                    return Encoding.ASCII.GetString(r).Trim();
                }
                catch (SocketException) { }
            }
            return null;
        }

        /// <summary>
        /// Read-only readiness check: sends one invitation and reads the challenge, but never authenticates, so it
        /// cannot count as a wrong-password attempt and never starts a transfer.
        /// </summary>
        public static OtaOutcome Probe(string host, int port, int size, string md5, Action<string> status)
        {
            IPAddress ip = ParseAddress(host);
            if (ip == null) return OtaOutcome.Make(OtaResult.BadArgument, "The pendant address is not valid.");
            using (UdpClient udp = new UdpClient(0))
            {
                string reply = Invite(udp, new IPEndPoint(ip, port), "0 40000 " + size + " " + md5 + "\n", 3, 2000, null, status);
                if (reply == null)
                    return OtaOutcome.Make(OtaResult.NoAnswer,
                        "No answer from the OTA service. The update is not armed (or the 5-minute window ended), the controller is not Idle, or the address is wrong.");
                if (reply.StartsWith("AUTH"))
                    return OtaOutcome.Make(OtaResult.Success, "The pendant is armed and asked for the password.");
                return OtaOutcome.Make(OtaResult.BadAnswer, "Unexpected answer from the pendant: " + Clean(reply));
            }
        }

        static string Clean(string s)
        {
            StringBuilder sb = new StringBuilder();
            foreach (char c in s) { if (c >= 32 && c < 127) sb.Append(c); if (sb.Length >= 60) break; }
            return sb.ToString();
        }

        /// <summary>Send the image. Blocks; call from a worker thread.</summary>
        public static OtaOutcome Upload(string host, int port, byte[] image, string password,
                                        Action<int> progress, Action<string> status, CancelFlag cancel,
                                        Action transferStarted)
        {
            IPAddress ip = ParseAddress(host);
            if (ip == null) return OtaOutcome.Make(OtaResult.BadArgument, "The pendant address is not valid.");
            if (image == null || image.Length == 0) return OtaOutcome.Make(OtaResult.BadArgument, "No firmware image.");
            if (string.IsNullOrEmpty(password)) return OtaOutcome.Make(OtaResult.BadArgument, "Enter the OTA password.");

            string fileMd5 = Md5Hex(image);
            IPEndPoint remote = new IPEndPoint(ip, port);

            TcpListener listener = null;
            int localPort = 0;
            Random rnd = new Random();
            for (int tries = 0; tries < 20 && listener == null; tries++)
            {
                localPort = rnd.Next(10000, 60000);
                try { TcpListener l = new TcpListener(IPAddress.Any, localPort); l.Start(1); listener = l; }
                catch (SocketException) { listener = null; }
            }
            if (listener == null)
                return OtaOutcome.Make(OtaResult.UploadError, "Could not open a local TCP port for the pendant to connect back to.");

            try
            {
                using (UdpClient udp = new UdpClient(0))
                {
                    string reply = Invite(udp, remote, "0 " + localPort + " " + image.Length + " " + fileMd5 + "\n", 8, 2000, cancel, status);
                    if (cancel != null && cancel.Requested) return OtaOutcome.Make(OtaResult.Cancelled, "Cancelled before the transfer started.");
                    if (reply == null)
                        return OtaOutcome.Make(OtaResult.NoAnswer,
                            "No answer from the pendant. Check that the update is ARMED on the touchscreen (SETTINGS > FIRMWARE UPDATE > ARM UPDATE), " +
                            "that the 5-minute window has not ended, that the controller is Idle, and that the address is right.");

                    if (reply.StartsWith("AUTH"))
                    {
                        string[] parts = reply.Split(' ');
                        if (parts.Length < 2 || parts[1].Length != 32)
                            return OtaOutcome.Make(OtaResult.BadAnswer, "The pendant sent a malformed challenge.");
                        string nonce = parts[1];
                        string cnonce = Md5HexText(Guid.NewGuid().ToString("N") + image.Length + fileMd5 + host);
                        string result = AuthResponse(password, nonce, cnonce);
                        if (status != null) status("Authenticating...");
                        byte[] auth = Encoding.ASCII.GetBytes("200 " + cnonce + " " + result + "\n");
                        udp.Send(auth, auth.Length, remote);
                        udp.Client.ReceiveTimeout = 10000;
                        string answer;
                        try
                        {
                            IPEndPoint from = new IPEndPoint(IPAddress.Any, 0);
                            answer = Encoding.ASCII.GetString(udp.Receive(ref from)).Trim();
                        }
                        catch (SocketException)
                        {
                            return OtaOutcome.Make(OtaResult.NoAnswer, "The pendant did not answer the authentication.");
                        }
                        if (answer != "OK")
                        {
                            if (answer.IndexOf("Authentication", StringComparison.OrdinalIgnoreCase) >= 0)
                                return OtaOutcome.Make(OtaResult.AuthFailed,
                                    "Wrong OTA password. (Five wrong passwords switch the update off on the pendant; arm it again.)");
                            return OtaOutcome.Make(OtaResult.BadAnswer, "Unexpected answer from the pendant: " + Clean(answer));
                        }
                        if (status != null) status("Password accepted.");
                    }
                    else if (reply != "OK")
                    {
                        return OtaOutcome.Make(OtaResult.BadAnswer, "Unexpected answer from the pendant: " + Clean(reply));
                    }
                }

                if (status != null) status("Waiting for the pendant to connect back to this PC...");
                TcpClient conn = null;
                DateTime until = DateTime.UtcNow.AddSeconds(15);
                while (DateTime.UtcNow < until)
                {
                    if (cancel != null && cancel.Requested) return OtaOutcome.Make(OtaResult.Cancelled, "Cancelled before the transfer started.");
                    if (listener.Pending()) { conn = listener.AcceptTcpClient(); break; }
                    Thread.Sleep(50);
                }
                if (conn == null)
                    return OtaOutcome.Make(OtaResult.DeviceNoConnect,
                        "The password was accepted but the pendant did not connect back. The usual cause is the Windows firewall blocking the incoming " +
                        "connection to FluidDial Updater (allow it on Private networks and try again). The pendant also refuses an image that does not fit.");

                using (conn)
                {
                    if (transferStarted != null) transferStarted();
                    if (status != null) status("Transferring " + image.Length + " bytes...");
                    NetworkStream ns = conn.GetStream();
                    ns.ReadTimeout = 10000;
                    ns.WriteTimeout = 10000;
                    byte[] ack = new byte[32];
                    bool lastOk = false;
                    int offset = 0;
                    int lastPct = -1;
                    try
                    {
                        while (offset < image.Length)
                        {
                            int n = Math.Min(ChunkSize, image.Length - offset);
                            ns.Write(image, offset, n);
                            offset += n;
                            int got = ns.Read(ack, 0, ack.Length);
                            if (got <= 0) throw new System.IO.IOException("connection closed");
                            lastOk = Encoding.ASCII.GetString(ack, 0, got).IndexOf("OK", StringComparison.Ordinal) >= 0;
                            int pct = (int)((long)offset * 100 / image.Length);
                            if (pct != lastPct && progress != null) { progress(pct); lastPct = pct; }
                        }
                        if (!lastOk)
                        {
                            if (status != null) status("Waiting for the pendant to verify the image...");
                            ns.ReadTimeout = 60000;
                            StringBuilder text = new StringBuilder();
                            for (int i = 0; i < 5 && !lastOk; i++)
                            {
                                int got = ns.Read(ack, 0, ack.Length);
                                if (got <= 0) break;
                                string s = Encoding.ASCII.GetString(ack, 0, got);
                                if (s.IndexOf("OK", StringComparison.Ordinal) >= 0) lastOk = true; else text.Append(Clean(s));
                            }
                            if (!lastOk)
                                return OtaOutcome.Make(OtaResult.UploadError,
                                    "The pendant did not accept the image" + (text.Length > 0 ? ": " + text : ".") + " The old firmware was kept.");
                        }
                    }
                    catch (Exception ex)
                    {
                        if (ex is System.IO.IOException || ex is SocketException || ex is ObjectDisposedException)
                            return OtaOutcome.Make(OtaResult.UploadError,
                                "The transfer was interrupted at " + (int)((long)offset * 100 / image.Length) + "%. The pendant keeps its old firmware " +
                                "(the new image is written to the other slot and is only used after it has been fully verified).");
                        throw;
                    }
                    if (progress != null) progress(100);
                }
                return OtaOutcome.Make(OtaResult.Success, "The pendant accepted the image.");
            }
            finally
            {
                listener.Stop();
            }
        }

        /// <summary>
        /// After a successful transfer the pendant checks the image and restarts about 3 seconds later. Watch ping:
        /// down then up again means it restarted. Returns a human-readable verdict and sets restarted.
        /// </summary>
        public static string WatchRestart(string host, Action<string> status, CancelFlag cancel, out bool restarted)
        {
            restarted = false;
            IPAddress ip = ParseAddress(host);
            if (ip == null) return "Cannot verify the restart (invalid address).";
            DateTime end = DateTime.UtcNow.AddSeconds(40);
            bool wentDown = false;
            if (status != null) status("Waiting for the pendant to restart...");
            while (DateTime.UtcNow < end)
            {
                if (cancel != null && cancel.Requested) return "Stopped waiting.";
                if (!PingOnce(ip, 400)) { wentDown = true; break; }
                Thread.Sleep(300);
            }
            if (!wentDown)
                return "The image was transferred but the pendant did NOT restart within 40 seconds. Look at the pendant screen: if it shows a failure " +
                       "(for example 'not a JC8048W550C_I image'), the old firmware was kept. If it shows 'restart pending', tap RESTART PENDANT.";
            if (status != null) status("The pendant is restarting...");
            end = DateTime.UtcNow.AddSeconds(120);
            while (DateTime.UtcNow < end)
            {
                if (cancel != null && cancel.Requested) return "Stopped waiting.";
                if (PingOnce(ip, 800))
                {
                    restarted = true;
                    return "The pendant restarted and is back on the network. Check the version in SETTINGS > FIRMWARE UPDATE on the touchscreen " +
                           "(the network cannot report it).";
                }
                Thread.Sleep(500);
            }
            return "The pendant went offline but did not return within 2 minutes. Check its screen and power; if it stays dark, use the USB recovery procedure.";
        }
    }
}
