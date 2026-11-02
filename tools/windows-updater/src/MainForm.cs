// FluidDial Updater - the window. Everything is built in code (no designer files) so the project is a handful of
// plain sources that the compiler built into Windows can build without any SDK.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace FluidDialUpdater
{
    public class MainForm : Form
    {
        readonly TextBox _ip = new TextBox();
        readonly ComboBox _fw = new ComboBox();
        readonly TextBox _pw = new TextBox();
        readonly Label _fwKind = new Label();
        readonly Label _fwInfo = new Label();
        readonly Label _fwVerdict = new Label();
        readonly Label _result = new Label();
        readonly Button _find = new Button();
        readonly Button _test = new Button();
        readonly Button _browse = new Button();
        readonly Button _update = new Button();
        readonly Button _cancel = new Button();
        readonly ProgressBar _bar = new ProgressBar();
        readonly TextBox _log = new TextBox();

        Manifest _manifest;
        FirmwareInfo _current;
        readonly List<string> _paths = new List<string>();
        Thread _worker;
        CancelFlag _cancelFlag = new CancelFlag();
        volatile bool _busy;
        volatile bool _transferring;

        public MainForm()
        {
            Text = "FluidDial Updater " + Program.Version;
            ClientSize = new Size(760, 700);
            MinimumSize = new Size(780, 740);
            Font = new Font("Segoe UI", 9f);
            StartPosition = FormStartPosition.CenterScreen;

            Label title = new Label();
            title.Text = "FluidDial Updater";
            title.Font = new Font("Segoe UI", 16f, FontStyle.Bold);
            title.SetBounds(16, 10, 500, 34);
            Controls.Add(title);
            Label sub = new Label();
            sub.Text = "Wi-Fi firmware update for the FluidDial-JC pendant (Guition JC8048W550C_I). Version " + Program.Version;
            sub.SetBounds(18, 44, 720, 20);
            Controls.Add(sub);

            Label steps = new Label();
            steps.Text = "1. On the pendant: SETTINGS > FIRMWARE UPDATE > ARM UPDATE (controller must be Idle).   2. Choose firmware.   3. Enter the OTA password.   4. Update.";
            steps.SetBounds(18, 66, 730, 34);
            steps.ForeColor = Color.DimGray;
            Controls.Add(steps);

            GroupBox g1 = Group("Pendant", 16, 106, 728, 62);
            Label l = new Label(); l.Text = "IP address"; l.SetBounds(12, 28, 70, 20); g1.Controls.Add(l);
            _ip.SetBounds(88, 25, 150, 24); g1.Controls.Add(_ip);
            _find.Text = "Find pendant"; _find.SetBounds(250, 23, 110, 28); _find.Click += OnFind; g1.Controls.Add(_find);
            _test.Text = "Test connection"; _test.SetBounds(368, 23, 130, 28); _test.Click += OnTest; g1.Controls.Add(_test);
            Label hint = new Label(); hint.Text = "Find works while the update is armed."; hint.ForeColor = Color.DimGray; hint.SetBounds(506, 28, 215, 20); g1.Controls.Add(hint);

            GroupBox g2 = Group("Firmware", 16, 176, 728, 196);
            _fw.DropDownStyle = ComboBoxStyle.DropDownList; _fw.SetBounds(12, 24, 598, 24); _fw.SelectedIndexChanged += delegate { SelectFirmware(); };
            g2.Controls.Add(_fw);
            _browse.Text = "Browse..."; _browse.SetBounds(618, 22, 98, 28); _browse.Click += OnBrowse; g2.Controls.Add(_browse);
            _fwKind.SetBounds(12, 58, 700, 26); _fwKind.Font = new Font("Segoe UI", 11f, FontStyle.Bold); g2.Controls.Add(_fwKind);
            _fwInfo.SetBounds(12, 88, 700, 56); g2.Controls.Add(_fwInfo);
            _fwVerdict.SetBounds(12, 148, 700, 40); g2.Controls.Add(_fwVerdict);

            GroupBox g3 = Group("OTA password", 16, 380, 728, 62);
            _pw.UseSystemPasswordChar = true; _pw.SetBounds(12, 26, 240, 24); _pw.TextChanged += delegate { UpdateButtons(); }; g3.Controls.Add(_pw);
            Label pl = new Label(); pl.Text = "The password set on the pendant. It is never saved or shown, and is not encrypted on the network."; pl.ForeColor = Color.DimGray; pl.SetBounds(262, 24, 455, 32); g3.Controls.Add(pl);

            _update.Text = "Update"; _update.SetBounds(16, 454, 160, 40); _update.Font = new Font("Segoe UI", 11f, FontStyle.Bold); _update.Click += OnUpdate; Controls.Add(_update);
            _cancel.Text = "Cancel"; _cancel.SetBounds(184, 454, 100, 40); _cancel.Click += delegate { _cancelFlag.Requested = true; }; _cancel.Enabled = false; Controls.Add(_cancel);
            _bar.SetBounds(300, 462, 444, 24); _bar.Minimum = 0; _bar.Maximum = 100; Controls.Add(_bar);

            _result.SetBounds(16, 502, 728, 48); _result.Font = new Font("Segoe UI", 12f, FontStyle.Bold); _result.Text = "Ready."; Controls.Add(_result);

            _log.Multiline = true; _log.ReadOnly = true; _log.ScrollBars = ScrollBars.Vertical; _log.SetBounds(16, 556, 728, 130);
            _log.BackColor = Color.White; Controls.Add(_log);

            _ip.Text = LoadIp();
            _manifest = Manifest.Load(Program.FirmwareDir());
            FillFirmwareList();
            UpdateButtons();
            FormClosing += OnClosing;
        }

        static GroupBox Group(string text, int x, int y, int w, int h)
        {
            GroupBox g = new GroupBox();
            g.Text = text;
            g.SetBounds(x, y, w, h);
            return g;
        }

        void Add(Control c) { Controls.Add(c); }

        // The only thing remembered between runs is the address. Never the password.
        static string SettingsFile()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "FluidDialUpdater", "address.txt");
        }

        static string LoadIp()
        {
            try { if (File.Exists(SettingsFile())) return File.ReadAllText(SettingsFile()).Trim(); } catch (Exception) { }
            return "192.168.0.135";
        }

        void SaveIp()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(SettingsFile()));
                File.WriteAllText(SettingsFile(), _ip.Text.Trim());
            }
            catch (Exception) { }
        }

        void Log(string s)
        {
            if (InvokeRequired) { BeginInvoke((Action)delegate { Log(s); }); return; }
            _log.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + s + "\r\n");
        }

        void Result(string s, Color c)
        {
            if (InvokeRequired) { BeginInvoke((Action)delegate { Result(s, c); }); return; }
            _result.Text = s;
            _result.ForeColor = c;
        }

        void FillFirmwareList()
        {
            _fw.Items.Clear();
            _paths.Clear();
            string dir = Program.FirmwareDir();
            if (Directory.Exists(dir))
            {
                string[] files = Directory.GetFiles(dir, "*.bin");
                Array.Sort(files);
                // Full-control first: it is the normal pendant firmware.
                Array.Sort(files, delegate(string a, string b)
                {
                    int ka = a.IndexOf("fullcontrol", StringComparison.OrdinalIgnoreCase) >= 0 ? 0 : 1;
                    int kb = b.IndexOf("fullcontrol", StringComparison.OrdinalIgnoreCase) >= 0 ? 0 : 1;
                    return ka != kb ? ka.CompareTo(kb) : string.Compare(a, b, StringComparison.OrdinalIgnoreCase);
                });
                foreach (string f in files)
                {
                    ManifestEntry e = _manifest.Find(Path.GetFileName(f));
                    string label = Path.GetFileName(f);
                    if (e != null) label += e.Kind == FirmwareKind.FullControl ? "   [FULL CONTROL]" : (e.Kind == FirmwareKind.Diagnostic ? "   [DIAGNOSTIC - motion disabled]" : "");
                    _fw.Items.Add(label);
                    _paths.Add(f);
                }
            }
            if (_fw.Items.Count > 0) _fw.SelectedIndex = 0; else SelectFirmware();
        }

        void SelectFirmware()
        {
            _current = null;
            int i = _fw.SelectedIndex;
            if (i < 0 || i >= _paths.Count)
            {
                _fwKind.Text = "No firmware selected";
                _fwKind.ForeColor = Color.Gray;
                _fwInfo.Text = "Use Browse... to choose a fluiddial-jc-*.bin application image.";
                _fwVerdict.Text = "";
                UpdateButtons();
                return;
            }
            ShowFirmware(FirmwareInfo.Analyze(_paths[i], _manifest));
        }

        void ShowFirmware(FirmwareInfo f)
        {
            _current = f;
            if (f.Compatible)
            {
                _fwKind.Text = f.KindText;
                _fwKind.ForeColor = f.Kind == FirmwareKind.Diagnostic ? Color.DarkOrange : Color.DarkGreen;
                _fwInfo.Text = "File: " + f.FileName + "\r\nVersion: " + (f.Version.Length > 0 ? f.Version : "unknown") +
                               (f.Commit.Length > 0 ? "   Source commit: " + f.Commit : "") + "   Size: " + f.Size + " bytes\r\nSHA-256: " + f.Sha256;
                _fwVerdict.Text = f.Verdict;
                _fwVerdict.ForeColor = f.InManifest ? Color.DarkGreen : Color.DarkOrange;
            }
            else
            {
                _fwKind.Text = "NOT COMPATIBLE";
                _fwKind.ForeColor = Color.Firebrick;
                _fwInfo.Text = "File: " + f.FileName;
                _fwVerdict.Text = f.Verdict;
                _fwVerdict.ForeColor = Color.Firebrick;
            }
            UpdateButtons();
        }

        void UpdateButtons()
        {
            bool idle = !_busy;
            _update.Enabled = idle && _current != null && _current.Compatible && _pw.Text.Length > 0 && OtaClient.IsValidAddress(_ip.Text);
            _find.Enabled = idle;
            _test.Enabled = idle;
            _browse.Enabled = idle;
            _fw.Enabled = idle;
            _ip.Enabled = idle;
            _pw.Enabled = idle;
            _cancel.Enabled = _busy && !_transferring;
        }

        void OnBrowse(object s, EventArgs e)
        {
            using (OpenFileDialog d = new OpenFileDialog())
            {
                d.Title = "Choose a FluidDial-JC firmware image";
                d.Filter = "Firmware images (*.bin)|*.bin";
                if (Directory.Exists(Program.FirmwareDir())) d.InitialDirectory = Program.FirmwareDir();
                if (d.ShowDialog(this) != DialogResult.OK) return;
                _paths.Add(d.FileName);
                _fw.Items.Add(d.FileName);
                _fw.SelectedIndex = _fw.Items.Count - 1;
            }
        }

        void OnFind(object s, EventArgs e)
        {
            Log("Looking for fluiddial-jc.local ...");
            Result("Looking for the pendant...", Color.Black);
            _busy = true; UpdateButtons();
            ThreadPool.QueueUserWorkItem(delegate
            {
                string ip = OtaClient.Discover("fluiddial-jc.local");
                BeginInvoke((Action)delegate
                {
                    _busy = false;
                    if (ip != null) { _ip.Text = ip; Log("Found the pendant at " + ip); Result("Pendant found at " + ip + ".", Color.DarkGreen); }
                    else
                    {
                        Log("fluiddial-jc.local did not resolve.");
                        Result("Pendant not found by name. It is only visible while the update is ARMED; otherwise type the IP shown on its FIRMWARE UPDATE page.", Color.DarkOrange);
                    }
                    UpdateButtons();
                });
            });
        }

        void OnTest(object s, EventArgs e)
        {
            string host = _ip.Text.Trim();
            if (!OtaClient.IsValidAddress(host)) { Result("Enter a valid pendant IP address.", Color.Firebrick); return; }
            int size = _current != null && _current.Compatible ? _current.Size : 1024;
            string md5 = _current != null && _current.Compatible ? _current.Md5 : "00000000000000000000000000000000";
            _busy = true; UpdateButtons();
            Result("Testing the connection...", Color.Black);
            ThreadPool.QueueUserWorkItem(delegate
            {
                string ping = OtaClient.PingText(host);
                Log("Ping " + host + ": " + ping);
                OtaOutcome o = OtaClient.Probe(host, OtaClient.DefaultPort, size, md5, null);
                Log(o.Message);
                BeginInvoke((Action)delegate
                {
                    _busy = false;
                    if (o.Ok) Result("Pendant reachable and ARMED. It is waiting for the password.", Color.DarkGreen);
                    else if (!ping.StartsWith("reply")) Result("The pendant does not answer on the network (" + ping + "). Check power, Wi-Fi and the address.", Color.Firebrick);
                    else Result("The pendant is on the network but the update is not armed. Arm it on the touchscreen.", Color.DarkOrange);
                    UpdateButtons();
                });
            });
        }

        void OnUpdate(object s, EventArgs e)
        {
            if (_current == null || !_current.Compatible) return;
            string host = _ip.Text.Trim();
            if (_current.Kind == FirmwareKind.Diagnostic)
            {
                DialogResult r = MessageBox.Show(this,
                    "This is the DIAGNOSTIC (motion disabled) firmware.\r\n\r\nInstalling it turns the pendant into a read-only/simulated one. It will NOT be able to jog or send machine commands until you install the full-control firmware again.\r\n\r\nInstall the diagnostic firmware?",
                    "Diagnostic firmware", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
                if (r != DialogResult.Yes) return;
            }
            else if (_current.Kind == FirmwareKind.Unknown)
            {
                DialogResult r = MessageBox.Show(this, "The kind of this FluidDial-JC image could not be determined. Install it anyway?", "Unknown firmware kind",
                    MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
                if (r != DialogResult.Yes) return;
            }
            SaveIp();
            string password = _pw.Text;
            FirmwareInfo fw = _current;
            _cancelFlag = new CancelFlag();
            CancelFlag flag = _cancelFlag;
            _busy = true; _transferring = false;
            _bar.Value = 0;
            Result("Updating - do not power off the pendant.", Color.Black);
            UpdateButtons();
            Log("Sending " + fw.FileName + " (" + fw.Size + " bytes) to " + host);

            _worker = new Thread(delegate()
            {
                OtaOutcome o = OtaClient.Upload(host, OtaClient.DefaultPort, fw.Data, password,
                    delegate(int p) { BeginInvoke((Action)delegate { _bar.Value = Math.Max(0, Math.Min(100, p)); }); },
                    delegate(string m) { Log(m); }, flag,
                    delegate { _transferring = true; BeginInvoke((Action)UpdateButtons); });
                _transferring = false;
                if (!o.Ok)
                {
                    Log(o.Message);
                    Result("FAILED: " + o.Message, Color.Firebrick);
                    Finish();
                    return;
                }
                Log(o.Message);
                bool restarted;
                string verdict = OtaClient.WatchRestart(host, delegate(string m) { Log(m); }, flag, out restarted);
                Log(verdict);
                if (restarted) Result("SUCCESS - update installed and the pendant restarted.\r\n" + "Check the version on the pendant.", Color.DarkGreen);
                else Result("Transferred, but the restart was not confirmed: " + verdict, Color.DarkOrange);
                Finish();
            });
            _worker.IsBackground = true;
            _worker.Start();
        }

        void Finish()
        {
            BeginInvoke((Action)delegate
            {
                _busy = false;
                _transferring = false;
                _pw.Text = "";   // do not leave the password sitting in the box
                UpdateButtons();
            });
        }

        void OnClosing(object sender, FormClosingEventArgs e)
        {
            if (_transferring)
            {
                MessageBox.Show(this, "A transfer is in progress. Wait for it to finish.", "Update running", MessageBoxButtons.OK, MessageBoxIcon.Information);
                e.Cancel = true;
                return;
            }
            _cancelFlag.Requested = true;
            SaveIp();
        }
    }
}
