using System.Net;
using System.Threading.Tasks;
using System.Windows;

namespace Sleepy
{
    public partial class SettingsWindow : Window
    {
        readonly ConfigStore _cfg;
        readonly Manager _mgr;

        public SettingsWindow(ConfigStore cfg, Manager mgr)
        {
            _cfg = cfg;
            _mgr = mgr;
            InitializeComponent();
            Native.UseDarkTitleBar(this);
            var c = cfg.Get();
            PcBox.Text = c.PcAddress;
            (c.Concurrency == 4 ? C4 : c.Concurrency == 3 ? C3 : c.Concurrency == 2 ? C2 : C1).IsChecked = true;
        }

        void Save_Click(object sender, RoutedEventArgs e)
        {
            string pc = PcBox.Text.Trim();
            IPAddress ip;
            if (pc.Length > 0 && (!IPAddress.TryParse(pc, out ip) || ip.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork))
            {
                Message.Text = "The PC address must be an IPv4 address such as 192.168.1.10, or empty for automatic.";
                return;
            }
            int n = C4.IsChecked == true ? 4 : C3.IsChecked == true ? 3 : C2.IsChecked == true ? 2 : 1;
            _cfg.Update(c => { c.Concurrency = n; c.PcAddress = pc; });
            Close();
        }

        void Cancel_Click(object sender, RoutedEventArgs e) { Close(); }

        void OpenLogs_Click(object sender, RoutedEventArgs e)
        {
            try { System.Diagnostics.Process.Start("explorer.exe", "\"" + AppLog.Dir + "\""); }
            catch { Message.Text = "Logs are in " + AppLog.Dir; }
        }

        void ConsoleLog_Click(object sender, RoutedEventArgs e)
        {
            Message.Text = "Fetching the console log...";
            Task.Run(() => _mgr.SaveConsoleLog("requested in settings")).ContinueWith(t => Dispatcher.BeginInvoke(new System.Action(() =>
            {
                if (t.Result == null) { Message.Text = "Could not get the console log. Is the console connected, and is the installer v1.10 or newer?"; return; }
                try { Clipboard.SetText(t.Result); Message.Text = "Console log copied to the clipboard and saved as console.log in the logs folder."; }
                catch { Message.Text = "Console log saved as console.log in the logs folder."; }
            })));
        }
    }
}
