// FluidDial Updater - entry point. No arguments: the window. "--cli": scripted use (also used by the tests).

using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows.Forms;

[assembly: AssemblyTitle("FluidDial Updater")]
[assembly: AssemblyProduct("FluidDial-JC firmware updater")]
[assembly: AssemblyVersion("1.5.3.0")]
[assembly: AssemblyFileVersion("1.5.3.0")]

namespace FluidDialUpdater
{
    static class Program
    {
        public const string Version = "1.5.3";

        [DllImport("kernel32.dll")]
        static extern bool AttachConsole(int pid);

        public static string FirmwareDir()
        {
            return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "firmware");
        }

        [STAThread]
        static int Main(string[] args)
        {
            if (args.Length > 0 && (args[0] == "--cli" || args[0] == "--analyze"))
            {
                AttachConsole(-1);
                return Cli.Run(args);
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
            return 0;
        }
    }

    static class Cli
    {
        static string Arg(string[] a, string name)
        {
            for (int i = 0; i < a.Length - 1; i++) if (a[i] == name) return a[i + 1];
            return null;
        }

        static bool Flag(string[] a, string name)
        {
            foreach (string s in a) if (s == name) return true;
            return false;
        }

        public static int Run(string[] a)
        {
            string file = Arg(a, "--file");
            string ip = Arg(a, "--ip");
            string portText = Arg(a, "--port");
            int port = portText == null ? OtaClient.DefaultPort : int.Parse(portText);
            Manifest manifest = Manifest.Load(Program.FirmwareDir());

            if (a[0] == "--analyze")
            {
                if (file == null) { Console.WriteLine("usage: --analyze --file <bin>"); return 2; }
                FirmwareInfo fi = FirmwareInfo.Analyze(file, manifest);
                Console.WriteLine((fi.Compatible ? "COMPATIBLE " : "REJECTED ") + fi.KindText + " | " + fi.Verdict);
                if (fi.Sha256 != null) Console.WriteLine("sha256 " + fi.Sha256 + " size " + fi.Size);
                return fi.Compatible ? 0 : 2;
            }

            if (ip == null || file == null) { Console.WriteLine("usage: --cli --ip <addr> [--port n] --file <bin> [--password-env VAR] [--probe] [--yes-diagnostic] [--no-restart-check]"); return 2; }
            FirmwareInfo f = FirmwareInfo.Analyze(file, manifest);
            if (!f.Compatible) { Console.WriteLine("REJECTED: " + f.Verdict); return 2; }

            Action<string> say = delegate(string s) { Console.WriteLine(s); };
            if (Flag(a, "--probe"))
            {
                OtaOutcome p = OtaClient.Probe(ip, port, f.Size, f.Md5, say);
                Console.WriteLine((p.Ok ? "PROBE OK: " : "PROBE FAILED (" + p.Result + "): ") + p.Message);
                return p.Ok ? 0 : 1;
            }
            if (f.Kind == FirmwareKind.Diagnostic && !Flag(a, "--yes-diagnostic"))
            {
                Console.WriteLine("REFUSED: this is the DIAGNOSTIC (motion disabled) image; pass --yes-diagnostic to confirm.");
                return 2;
            }
            string envName = Arg(a, "--password-env");
            string password = envName == null ? null : Environment.GetEnvironmentVariable(envName);
            int lastPct = -10;
            Action<int> prog = delegate(int p) { if (p >= lastPct + 10 || p == 100) { Console.WriteLine("progress " + p + "%"); lastPct = p; } };
            OtaOutcome o = OtaClient.Upload(ip, port, f.Data, password, prog, say, new CancelFlag(), null);
            Console.WriteLine((o.Ok ? "TRANSFER OK: " : "FAILED (" + o.Result + "): ") + o.Message);
            if (!o.Ok) return 1;
            if (!Flag(a, "--no-restart-check"))
            {
                bool restarted;
                string v = OtaClient.WatchRestart(ip, say, new CancelFlag(), out restarted);
                Console.WriteLine((restarted ? "RESTART OK: " : "RESTART NOT CONFIRMED: ") + v);
                return restarted ? 0 : 3;
            }
            return 0;
        }
    }
}
