using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace Sleepy
{
    public partial class MainWindow : Window, INotifyPropertyChanged
    {
        readonly ConfigStore _cfg;
        readonly Library _lib;
        readonly FileServer _fs;
        readonly Manager _mgr;

        readonly Dictionary<string, HashSet<string>> _custom = new Dictionary<string, HashSet<string>>();
        readonly ObservableCollection<JobVM> _jobs = new ObservableCollection<JobVM>();
        readonly Dictionary<int, JobVM> _jobMap = new Dictionary<int, JobVM>();
        readonly DispatcherTimer _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        readonly DispatcherTimer _toastTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3.5) };
        ObservableCollection<GameVM> _games = new ObservableCollection<GameVM>();
        Dictionary<string, bool> _installed = new Dictionary<string, bool>();
        string _filter = "all", _sort = "name";
        int _libVersion = -1;
        bool _ready, _connected;
        bool _openedConnect;

        public MainWindow(ConfigStore cfg, Library lib, FileServer fs, Manager mgr)
        {
            _cfg = cfg; _lib = lib; _fs = fs; _mgr = mgr;
            InitializeComponent();
            Native.UseDarkTitleBar(this);

            JobsView = new ListCollectionView(_jobs) { IsLiveSorting = true };
            ((ListCollectionView)JobsView).LiveSortingProperties.Add("SortKey");
            JobsView.SortDescriptions.Add(new SortDescription("SortKey", ListSortDirection.Ascending));
            JobsView.SortDescriptions.Add(new SortDescription("Id", ListSortDirection.Ascending));

            DataContext = this;
            _ready = true;
            ApplyGames(new ObservableCollection<GameVM>());

            _timer.Tick += (s, e) => Refresh();
            _timer.Start();
            _toastTimer.Tick += (s, e) => { _toastTimer.Stop(); ToastVisibility = Visibility.Collapsed; };
            _lib.Changed += () => Dispatcher.BeginInvoke(new Action(OnLibraryChanged));

            Loaded += (s, e) =>
            {
                Task.Run(() => _lib.Scan(_cfg.Get().Folders));
                var c = _cfg.Get();
                if (c.AutoConnect && c.ConsoleHost.Length > 0)
                    Task.Run(() => { try { _mgr.Connect(c.ConsoleHost, c.ConsolePort); } catch { } });
            };
            Closing += OnClosing;
        }

        // ---------- bindable state ----------

        public event PropertyChangedEventHandler PropertyChanged;
        void Set<T>(ref T field, T value, [CallerMemberName] string name = null)
        {
            if (EqualityComparer<T>.Default.Equals(field, value)) return;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }

        ICollectionView _gamesView; public ICollectionView GamesView { get { return _gamesView; } private set { Set(ref _gamesView, value); } }
        public ICollectionView JobsView { get; private set; }
        string _conText = "Connect"; public string ConText { get { return _conText; } private set { Set(ref _conText, value); } }
        Brush _conDot = Brushes2.Err; public Brush ConDot { get { return _conDot; } private set { Set(ref _conDot, value); } }
        Visibility _scanVis = Visibility.Collapsed; public Visibility ScanVisibility { get { return _scanVis; } private set { Set(ref _scanVis, value); } }
        double _scanPct; public double ScanPercent { get { return _scanPct; } private set { Set(ref _scanPct, value); } }
        string _scanText = ""; public string ScanText { get { return _scanText; } private set { Set(ref _scanText, value); } }
        Visibility _emptyVis = Visibility.Collapsed; public Visibility EmptyVisibility { get { return _emptyVis; } private set { Set(ref _emptyVis, value); } }
        string _emptyText = ""; public string EmptyText { get { return _emptyText; } private set { Set(ref _emptyText, value); } }
        Visibility _addVis = Visibility.Collapsed; public Visibility AddFolderVisibility { get { return _addVis; } private set { Set(ref _addVis, value); } }
        Visibility _selVis = Visibility.Collapsed; public Visibility SelVisibility { get { return _selVis; } private set { Set(ref _selVis, value); } }
        string _selText = ""; public string SelText { get { return _selText; } private set { Set(ref _selText, value); } }
        bool _canSendSel; public bool CanSendSelected { get { return _canSendSel; } private set { Set(ref _canSendSel, value); } }
        string _speed = "Idle"; public string SpeedText { get { return _speed; } private set { Set(ref _speed, value); } }
        Visibility _ovVis = Visibility.Collapsed; public Visibility OverallVisibility { get { return _ovVis; } private set { Set(ref _ovVis, value); } }
        double _ovPct; public double OverallPercent { get { return _ovPct; } private set { Set(ref _ovPct, value); } }
        string _ovText = ""; public string OverallText { get { return _ovText; } private set { Set(ref _ovText, value); } }
        Visibility _qEmpty = Visibility.Visible; public Visibility QueueEmptyVisibility { get { return _qEmpty; } private set { Set(ref _qEmpty, value); } }
        string _pcInfo = ""; public string PcInfo { get { return _pcInfo; } private set { Set(ref _pcInfo, value); } }
        string _toast = ""; public string ToastText { get { return _toast; } private set { Set(ref _toast, value); } }
        Visibility _toastVis = Visibility.Collapsed; public Visibility ToastVisibility { get { return _toastVis; } private set { Set(ref _toastVis, value); } }
        Brush _toastBg = new SolidColorBrush(Color.FromRgb(0x23, 0x30, 0x47)); public Brush ToastBg { get { return _toastBg; } private set { Set(ref _toastBg, value); } }

        public void Toast(string text, bool error = false)
        {
            ToastText = text;
            ToastBg = new SolidColorBrush(error ? Color.FromRgb(0x3A, 0x1B, 0x1B) : Color.FromRgb(0x23, 0x30, 0x47));
            ToastVisibility = Visibility.Visible;
            _toastTimer.Stop(); _toastTimer.Start();
        }

        // ---------- library ----------

        void OnLibraryChanged()
        {
            if (_lib.Version == _libVersion) return;
            _libVersion = _lib.Version;

            foreach (var old in _games) { var ids = old.CustomIds; if (ids != null) _custom[old.TitleId] = new HashSet<string>(ids); }
            var selected = new HashSet<string>(_games.Where(g => g.IsSelected).Select(g => g.TitleId));

            var list = new ObservableCollection<GameVM>();
            foreach (var g in _lib.Groups)
            {
                HashSet<string> custom;
                _custom.TryGetValue(g.TitleId, out custom);
                var vm = new GameVM(g, custom);
                bool inst;
                if (_connected && g.Base != null && _installed.TryGetValue(g.TitleId, out inst)) vm.SetInstalled(inst);
                vm.IsSelected = selected.Contains(g.TitleId);
                list.Add(vm);
            }
            ApplyGames(list);
        }

        void ApplyGames(ObservableCollection<GameVM> list)
        {
            foreach (var g in _games) g.Changed -= UpdateSelection;
            _games = list;
            foreach (var g in _games) g.Changed += UpdateSelection;

            var view = new ListCollectionView(_games) { Filter = FilterGame };
            GamesView = view;
            ApplySort();
            UpdateSelection();
            UpdateEmpty();
        }

        bool FilterGame(object o)
        {
            var g = (GameVM)o;
            string q = SearchBox.Text.Trim();
            if (q.Length > 0 && g.Title.IndexOf(q, StringComparison.CurrentCultureIgnoreCase) < 0 && g.TitleId.IndexOf(q, StringComparison.OrdinalIgnoreCase) < 0) return false;
            switch (_filter)
            {
                case "new": return g.IsNew;
                case "update": return g.HasUpdate;
                case "dlc": return g.HasDlc;
            }
            return true;
        }

        void ApplySort()
        {
            var v = GamesView as ListCollectionView;
            if (v == null) return;
            v.SortDescriptions.Clear();
            switch (_sort)
            {
                case "size": v.SortDescriptions.Add(new SortDescription("SizeBytes", ListSortDirection.Descending)); break;
                case "recent": v.SortDescriptions.Add(new SortDescription("UpdatedTicks", ListSortDirection.Descending)); break;
                default: v.SortDescriptions.Add(new SortDescription("Title", ListSortDirection.Ascending)); break;
            }
        }

        void UpdateEmpty()
        {
            var v = GamesView as ListCollectionView;
            bool none = v == null || v.Count == 0;
            bool scanning = _lib.Status.Scanning;
            EmptyVisibility = none && !scanning ? Visibility.Visible : Visibility.Collapsed;
            bool noFolders = _cfg.Get().Folders.Count == 0;
            AddFolderVisibility = noFolders ? Visibility.Visible : Visibility.Collapsed;
            EmptyText = noFolders ? "Add a folder with your PS4 packages.\nGames, updates and DLC are grouped automatically.\nYou can also drag folders onto this window."
                                  : _games.Count > 0 ? "No games match your search or filter." : "No packages were found in your folders.";
        }

        void UpdateSelection()
        {
            var sel = _games.Where(g => g.IsSelected).ToList();
            if (sel.Count == 0) { SelVisibility = Visibility.Collapsed; return; }
            var pk = sel.SelectMany(g => g.Plan()).ToList();
            SelVisibility = Visibility.Visible;
            CanSendSelected = pk.Count > 0;
            SelText = sel.Count + (sel.Count == 1 ? " game selected · " : " games selected · ") + pk.Count + (pk.Count == 1 ? " package · " : " packages · ") + Fmt.Size(pk.Sum(p => p.Size));
        }

        // ---------- periodic refresh ----------

        void Refresh()
        {
            ConsoleState st; List<Job> jobs; Dictionary<string, bool> installed; long rx; int serving;
            _mgr.Snapshot(out st, out jobs, out installed, out rx, out serving);

            _connected = st.Connected;
            _installed = installed;
            ConText = st.Connected ? "PS4 · " + st.Host : (st.Error.Length > 0 ? "Reconnecting…" : "Connect");
            ConDot = st.Connected ? Brushes2.Ok : st.Error.Length > 0 ? Brushes2.Warn : Brushes2.Err;

            // installed state of every game
            bool changed = false;
            foreach (var g in _games)
            {
                if (g.Group.Base == null) continue;
                bool v;
                bool? now = st.Connected && installed.TryGetValue(g.TitleId, out v) ? v : (bool?)null;
                if (g.Installed != now) { g.SetInstalled(now); changed = true; }
            }
            if (changed && GamesView != null) GamesView.Refresh();

            // scan progress
            var sc = _lib.Status;
            ScanVisibility = sc.Scanning ? Visibility.Visible : Visibility.Collapsed;
            ScanPercent = sc.Total > 0 ? sc.Done * 100.0 / sc.Total : 0;
            ScanText = sc.Total > 0 ? "Scanning packages…  " + sc.Done + " / " + sc.Total : "Looking for packages…";
            UpdateEmpty();

            // jobs
            var ids = new HashSet<int>(jobs.Select(j => j.Id));
            foreach (var gone in _jobMap.Keys.Where(k => !ids.Contains(k)).ToList()) { _jobs.Remove(_jobMap[gone]); _jobMap.Remove(gone); }
            foreach (var j in jobs)
            {
                JobVM vm;
                if (!_jobMap.TryGetValue(j.Id, out vm)) { vm = new JobVM(j.Id); _jobMap[j.Id] = vm; _jobs.Add(vm); }
                vm.Update(j);
            }
            QueueEmptyVisibility = jobs.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

            var live = jobs.Where(j => j.State != JobState.Canceled).ToList();
            long speed = jobs.Where(j => j.State == JobState.Downloading).Sum(j => j.Speed);
            SpeedText = speed > 0 ? Fmt.Rate(speed) : rx > 0 ? Fmt.Rate(rx) : "Idle";
            if (live.Count > 0)
            {
                double total = live.Sum(j => (double)j.Size);
                double done = live.Sum(j => j.State == JobState.Done ? (double)j.Size : j.Size * j.Percent / 100.0);
                int pct = total > 0 ? (int)(done * 100 / total) : 0;
                OverallVisibility = Visibility.Visible;
                OverallPercent = pct;
                OverallText = live.Count(j => j.State == JobState.Done) + " of " + live.Count + " packages · " + pct + "% · " + Fmt.Size((long)done) + " / " + Fmt.Size((long)total);
            }
            else OverallVisibility = Visibility.Collapsed;

            string pc = st.Connected ? _mgr.PcAddress() : "";
            PcInfo = pc.Length > 0 ? "This PC: " + pc + ":" + _fs.Port : "";

            if (!_openedConnect && !st.Connected && st.Host.Length == 0 && _lib.Version >= 0)
            {
                _openedConnect = true;
                Connect_Click(null, null);
            }
        }

        // ---------- actions ----------

        void Send(IEnumerable<GameVM> games)
        {
            if (!_connected) { Toast("Connect to your console first.", true); Connect_Click(null, null); return; }
            var list = games.ToList();
            var ids = list.SelectMany(g => g.Plan()).Select(p => p.Id).ToList();
            if (ids.Count == 0) { Toast("Nothing to send for this selection.", true); return; }
            foreach (var g in list) g.IsSelected = false;

            Task.Run(() =>
            {
                try
                {
                    int n = _mgr.Enqueue(ids);
                    Dispatcher.BeginInvoke(new Action(() => Toast(n > 0 ? "Queued " + n + (n == 1 ? " package" : " packages") : "Already in the queue")));
                }
                catch (Exception ex) { Dispatcher.BeginInvoke(new Action(() => Toast(ex.Message, true))); }
            });
        }

        void AddFolder(string path)
        {
            if (!Directory.Exists(path)) return;
            _cfg.Update(c => { if (!c.Folders.Any(f => string.Equals(f, path, StringComparison.OrdinalIgnoreCase))) c.Folders.Add(path); });
            Task.Run(() => _lib.Scan(_cfg.Get().Folders));
        }

        static GameVM Vm(object sender) { return (GameVM)((FrameworkElement)sender).DataContext; }

        void Card_MouseUp(object sender, MouseButtonEventArgs e)
        {
            // clicking anywhere on the card (but not on its buttons/checkboxes) toggles the selection
            var src = e.OriginalSource as DependencyObject;
            while (src != null && !(src is Border && ((Border)src).Name == "Card"))
            {
                if (src is ButtonBase) return;
                src = (src is Visual || src is System.Windows.Media.Media3D.Visual3D) ? VisualTreeHelper.GetParent(src) : LogicalTreeHelper.GetParent(src);
            }
            var vm = Vm(sender);
            vm.IsSelected = !vm.IsSelected;
        }

        void Expand_Click(object sender, RoutedEventArgs e) { var vm = Vm(sender); vm.IsExpanded = !vm.IsExpanded; }
        void SendOne_Click(object sender, RoutedEventArgs e) { Send(new[] { Vm(sender) }); }
        void SendSelected_Click(object sender, RoutedEventArgs e) { Send(_games.Where(g => g.IsSelected)); }
        void SelectAll_Click(object sender, RoutedEventArgs e) { foreach (var g in GamesView.Cast<GameVM>().ToList()) g.IsSelected = true; }
        void SelectNone_Click(object sender, RoutedEventArgs e) { foreach (var g in _games) g.IsSelected = false; }

        void Filter_Checked(object sender, RoutedEventArgs e)
        {
            if (!_ready) return;
            _filter = (string)((RadioButton)sender).Tag;
            GamesView.Refresh();
            UpdateEmpty();
        }

        void Sort_Checked(object sender, RoutedEventArgs e)
        {
            if (!_ready) return;
            _sort = (string)((RadioButton)sender).Tag;
            ApplySort();
        }

        void Search_Changed(object sender, TextChangedEventArgs e)
        {
            if (!_ready) return;
            GamesView.Refresh();
            UpdateEmpty();
        }

        void Job_Click(object sender, RoutedEventArgs e)
        {
            var b = (FrameworkElement)sender;
            int id = ((JobVM)b.DataContext).Id;
            string op = (string)b.Tag;
            Task.Run(() =>
            {
                try { _mgr.JobOp(id, op); }
                catch (Exception ex) { Dispatcher.BeginInvoke(new Action(() => Toast(ex.Message, true))); }
            });
        }

        void CopyError_Click(object sender, RoutedEventArgs e)
        {
            var vm = (JobVM)((FrameworkElement)sender).DataContext;
            try
            {
                Clipboard.SetText(string.IsNullOrEmpty(vm.Detail) ? vm.Error : vm.Detail);
                Toast("Error details copied to the clipboard.");
            }
            catch { Toast("Could not access the clipboard.", true); }
        }

        void PauseAll_Click(object sender, RoutedEventArgs e) { Task.Run(() => _mgr.BulkOp("pause")); }
        void ResumeAll_Click(object sender, RoutedEventArgs e) { Task.Run(() => _mgr.BulkOp("resume")); }
        void ClearDone_Click(object sender, RoutedEventArgs e) { _mgr.ClearFinished(); }

        void Connect_Click(object sender, RoutedEventArgs e)
        {
            var w = new ConnectWindow(_mgr, _cfg) { Owner = this };
            w.ShowDialog();
            if (w.ConnectedHost != null) Toast("Connected to " + w.ConnectedHost);
        }

        void Folders_Click(object sender, RoutedEventArgs e)
        {
            new FoldersWindow(_cfg, _lib) { Owner = this }.ShowDialog();
            UpdateEmpty();
        }

        void Settings_Click(object sender, RoutedEventArgs e) { new SettingsWindow(_cfg) { Owner = this }.ShowDialog(); }

        void Window_Drop(object sender, DragEventArgs e)
        {
            var files = e.Data.GetData(DataFormats.FileDrop) as string[];
            if (files == null) return;
            int n = 0;
            foreach (var f in files)
            {
                if (Directory.Exists(f)) { AddFolder(f); n++; }
                else if (File.Exists(f) && string.Equals(Path.GetExtension(f), ".pkg", StringComparison.OrdinalIgnoreCase))
                {
                    var dir = Path.GetDirectoryName(f);
                    if (dir != null) { AddFolder(dir); n++; }
                }
            }
            if (n > 0) Toast("Scanning dropped folder" + (n > 1 ? "s" : "") + "…");
        }

        void OnClosing(object sender, CancelEventArgs e)
        {
            if (_mgr.Busy)
            {
                var r = MessageBox.Show(this, "Transfers are still running. If you close the app now, the console can't finish downloading.\n\nClose anyway?",
                    App.WindowTitle, MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No);
                if (r != MessageBoxResult.Yes) { e.Cancel = true; return; }
            }
            _timer.Stop();
        }
    }
}
