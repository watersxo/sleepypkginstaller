using System.Net;
using System.Windows;

namespace Sleepy
{
    public partial class SettingsWindow : Window
    {
        readonly ConfigStore _cfg;

        public SettingsWindow(ConfigStore cfg)
        {
            _cfg = cfg;
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
    }
}
