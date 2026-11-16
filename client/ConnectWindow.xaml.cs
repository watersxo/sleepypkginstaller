using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace Sleepy
{
    public partial class ConnectWindow : Window
    {
        readonly Manager _mgr;
        readonly ConfigStore _cfg;

        /// <summary>Set when this dialog connected to a console.</summary>
        public string ConnectedHost { get; private set; }

        public ConnectWindow(Manager mgr, ConfigStore cfg)
        {
            _mgr = mgr; _cfg = cfg;
            InitializeComponent();
            Native.UseDarkTitleBar(this);

            var c = cfg.Get();
            HostBox.Text = c.ConsoleHost;
            PortBox.Text = c.ConsolePort.ToString();
            Loaded += (s, e) => { ShowState(); HostBox.Focus(); HostBox.SelectAll(); };
        }

        void ShowState()
        {
            ConsoleState st; List<Job> jobs; Dictionary<string, bool> inst; long rx; int serving;
            _mgr.Snapshot(out st, out jobs, out inst, out rx, out serving);

            DisconnectedPanel.Visibility = st.Connected ? Visibility.Collapsed : Visibility.Visible;
            ConnectedPanel.Visibility = st.Connected ? Visibility.Visible : Visibility.Collapsed;
            if (st.Connected)
            {
                ConnectedText.Text = st.Host + ":" + st.Port + (st.Sleepy ? "" : "   (older installer)");
                ConnectedDetail.Text = (string.IsNullOrEmpty(st.Message) ? "Ready" : st.Message) + " · " + st.Tasks + (st.Tasks == 1 ? " task" : " tasks")
                    + " on the console.\nThe console downloads packages from this PC at " + _mgr.PcAddress() + ".";
            }
        }

        void SetBusy(bool busy, string message)
        {
            ConnectBtn.IsEnabled = FindBtn.IsEnabled = HostBox.IsEnabled = PortBox.IsEnabled = !busy;
            Message.Foreground = (System.Windows.Media.Brush)FindResource("Dim");
            Message.Text = message ?? "";
        }

        async Task DoConnect(string host, int port)
        {
            SetBusy(true, "Connecting…");
            try
            {
                await Task.Run(() => _mgr.Connect(host, port));
                ConnectedHost = host;
                Close();
            }
            catch (Exception ex)
            {
                SetBusy(false, null);
                Message.Foreground = (System.Windows.Media.Brush)FindResource("Err");
                Message.Text = ex.Message;
            }
        }

        void Host_KeyDown(object sender, KeyEventArgs e) { if (e.Key == Key.Enter) Connect_Click(null, null); }

        async void Connect_Click(object sender, RoutedEventArgs e)
        {
            int port;
            if (!int.TryParse(PortBox.Text.Trim(), out port)) port = 12800;
            await DoConnect(HostBox.Text, port);
        }

        async void Find_Click(object sender, RoutedEventArgs e)
        {
            SetBusy(true, "Looking for consoles on your network…");
            FoundList.ItemsSource = null;
            List<FoundConsole> found;
            try { found = await Task.Run(() => _mgr.Console.Discover()); }
            catch (Exception ex) { SetBusy(false, "Search failed: " + ex.Message); return; }

            if (found.Count == 0)
            {
                SetBusy(false, "Nothing found. Make sure the app is open on the console and both are on the same network, or type the IP address.");
                return;
            }
            if (found.Count == 1) { await DoConnect(found[0].Host, found[0].Port); return; }
            SetBusy(false, "Choose your console:");
            FoundList.ItemsSource = found;
        }

        async void Found_Click(object sender, RoutedEventArgs e)
        {
            var f = (FoundConsole)((Button)sender).Tag;
            await DoConnect(f.Host, f.Port);
        }

        void Refresh_Click(object sender, RoutedEventArgs e)
        {
            Task.Run(() => _mgr.RefreshInstalled());
            Message.Text = "Checking which games are installed…";
        }

        void Disconnect_Click(object sender, RoutedEventArgs e)
        {
            _mgr.Disconnect();
            ShowState();
            Message.Text = "";
        }

        void Close_Click(object sender, RoutedEventArgs e) { Close(); }
    }
}
