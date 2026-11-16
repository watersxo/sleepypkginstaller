using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Sleepy
{
    public abstract class Notifier : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;
        protected bool Set<T>(ref T field, T value, [CallerMemberName] string name = null)
        {
            if (EqualityComparer<T>.Default.Equals(field, value)) return false;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
            return true;
        }
        protected void Raise([CallerMemberName] string name = null) { PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name)); }
    }

    public static class Fmt
    {
        public static string Size(long n)
        {
            if (n >= 1L << 30) return (n / (double)(1L << 30)).ToString(n >= 10L << 30 ? "0.0" : "0.00") + " GB";
            if (n >= 1L << 20) return (n / (double)(1L << 20)).ToString(n >= 100L << 20 ? "0" : "0.0") + " MB";
            return Math.Max(1, n / 1024) + " KB";
        }

        public static string Rate(long n)
        {
            double mb = n / (double)(1 << 20);
            return mb.ToString(mb >= 10 ? "0" : "0.0") + " MB/s";
        }

        public static string Eta(int s)
        {
            if (s <= 0) return "";
            int h = s / 3600, m = s / 60 % 60;
            return h > 0 ? h + "h " + m + "m left" : m > 0 ? m + "m " + s % 60 + "s left" : s + "s left";
        }
    }

    static class Brushes2
    {
        static SolidColorBrush B(string hex) { var b = (SolidColorBrush)new BrushConverter().ConvertFromString(hex); b.Freeze(); return b; }
        public static readonly Brush Accent = B("#4F8DF0"), Ok = B("#4CC38A"), Warn = B("#E5B567"), Err = B("#EF6B6B"), Dim = B("#8B98A8");
    }

    public class BadgeVM
    {
        public string Text { get; set; }
        public string Kind { get; set; } // ok, update, dlc, err, plain
    }

    public class PartVM : Notifier
    {
        readonly GameVM _owner;
        bool _checked;
        public PkgInfo Pkg { get; }
        public string Label { get; }
        public string SizeText { get { return Fmt.Size(Pkg.Size); } }
        public bool IsEnabled { get { return !Pkg.Incomplete; } }

        public PartVM(GameVM owner, PkgInfo pkg)
        {
            _owner = owner; Pkg = pkg;
            string name = pkg.Kind == Kinds.Base ? "Game" : pkg.Kind == Kinds.Update ? "Update " + pkg.Version : "DLC: " + pkg.Title;
            Label = name + (pkg.Incomplete ? " (incomplete file)" : "");
        }

        public bool IsChecked
        {
            get { return _checked; }
            set { if (Set(ref _checked, value)) _owner.PartChanged(this); }
        }

        public void SetSilently(bool v) { if (_checked != v) { _checked = v; Raise("IsChecked"); } }
    }

    /// <summary>One game card in the library.</summary>
    public class GameVM : Notifier
    {
        bool _selected, _expanded;
        bool? _installed;
        bool _custom;
        ImageSource _icon;
        bool _iconTried;
        string _planText = "";
        bool _canSend;
        public event Action Changed;

        public GameGroup Group { get; }
        public string TitleId { get { return Group.TitleId; } }
        public string Title { get { return Group.Title; } }
        public ObservableCollection<PartVM> Parts { get; } = new ObservableCollection<PartVM>();
        public ObservableCollection<BadgeVM> Badges { get; } = new ObservableCollection<BadgeVM>();

        public GameVM(GameGroup g, ICollection<string> customIds)
        {
            Group = g;
            foreach (var p in g.Parts()) Parts.Add(new PartVM(this, p));
            if (customIds != null)
            {
                _custom = true;
                foreach (var pv in Parts) pv.SetSilently(customIds.Contains(pv.Pkg.Id) && !pv.Pkg.Incomplete);
            }
            else ApplyDefaults();
            Refresh();
        }

        public string Meta
        {
            get
            {
                var g = Group;
                return g.TitleId + " · " + g.Region + (g.Base != null ? " · v" + g.Base.Version : "") + " · " + Fmt.Size(g.Size);
            }
        }

        public ImageSource Icon
        {
            get
            {
                if (_icon == null && !_iconTried)
                {
                    _iconTried = true;
                    try
                    {
                        if (Group.IconPath != null && File.Exists(Group.IconPath))
                        {
                            var bmp = new BitmapImage();
                            bmp.BeginInit();
                            bmp.CacheOption = BitmapCacheOption.OnLoad;
                            bmp.DecodePixelWidth = 160;
                            bmp.UriSource = new Uri(Group.IconPath);
                            bmp.EndInit();
                            bmp.Freeze();
                            _icon = bmp;
                        }
                    }
                    catch { }
                }
                return _icon;
            }
        }

        public bool IsSelected { get { return _selected; } set { if (Set(ref _selected, value)) Changed?.Invoke(); } }
        public bool IsExpanded { get { return _expanded; } set { Set(ref _expanded, value); Raise("ExpandText"); } }
        public string ExpandText { get { return _expanded ? "Hide contents" : "Contents"; } }
        public bool? Installed { get { return _installed; } }
        public string PlanText { get { return _planText; } private set { Set(ref _planText, value); } }
        public bool CanSend { get { return _canSend; } private set { Set(ref _canSend, value); } }
        public bool HasUpdate { get { return Group.Updates.Count > 0; } }
        public bool HasDlc { get { return Group.Dlc.Count > 0; } }
        public bool IsNew { get { return Group.Base != null && _installed != true; } }
        public long SizeBytes { get { return Group.Size; } }
        public long UpdatedTicks { get { return Group.Updated; } }

        public IEnumerable<string> CustomIds { get { return _custom ? Parts.Where(p => p.IsChecked).Select(p => p.Pkg.Id).ToList() : null; } }

        public void SetInstalled(bool? v)
        {
            if (_installed == v) return;
            _installed = v;
            if (!_custom) ApplyDefaults();
            Refresh();
            Raise("Installed"); Raise("IsNew");
            Changed?.Invoke();
        }

        // base (unless installed), newest update and all DLC
        void ApplyDefaults()
        {
            bool installed = _installed == true;
            bool first = true;
            foreach (var pv in Parts)
            {
                bool on;
                switch (pv.Pkg.Kind)
                {
                    case Kinds.Base: on = !installed; break;
                    case Kinds.Update: on = first; first = false; break;
                    default: on = true; break;
                }
                pv.SetSilently(on && !pv.Pkg.Incomplete);
            }
        }

        public void PartChanged(PartVM pv)
        {
            _custom = true;
            Refresh();
            Changed?.Invoke();
        }

        /// <summary>The packages "Send" installs for this game, in install order.</summary>
        public List<PkgInfo> Plan()
        {
            return Parts.Where(p => p.IsChecked && !p.Pkg.Incomplete).Select(p => p.Pkg)
                        .OrderBy(p => p.Kind == Kinds.Base ? 0 : p.Kind == Kinds.Update ? 1 : 2)
                        .ThenBy(p => p.Version, Comparer<string>.Create(PkgReader.CompareVersions)).ToList();
        }

        void Refresh()
        {
            var plan = Plan();
            CanSend = plan.Count > 0;
            PlanText = plan.Count == 0 ? "Nothing to send" : plan.Count + (plan.Count == 1 ? " package · " : " packages · ") + Fmt.Size(plan.Sum(p => p.Size));

            var b = new List<BadgeVM>();
            if (_installed == true) b.Add(new BadgeVM { Text = "Installed", Kind = "ok" });
            if (Group.Orphan) b.Add(new BadgeVM { Text = "Game missing", Kind = "err" });
            if (Group.Updates.Count > 0)
                b.Add(new BadgeVM { Text = "Update " + Group.Updates[0].Version + (Group.Updates.Count > 1 ? " (+" + (Group.Updates.Count - 1) + " older)" : ""), Kind = "update" });
            if (Group.Dlc.Count > 0) b.Add(new BadgeVM { Text = Group.Dlc.Count + " DLC", Kind = "dlc" });
            if (Group.Base != null && Group.Base.Incomplete) b.Add(new BadgeVM { Text = "Incomplete file", Kind = "err" });
            if (Group.Duplicates > 0) b.Add(new BadgeVM { Text = Group.Duplicates + (Group.Duplicates == 1 ? " duplicate ignored" : " duplicates ignored"), Kind = "plain" });

            // only touch the collection when something changed, to avoid flicker
            if (!b.Select(x => x.Text + x.Kind).SequenceEqual(Badges.Select(x => x.Text + x.Kind)))
            {
                Badges.Clear();
                foreach (var x in b) Badges.Add(x);
            }
        }
    }

    /// <summary>One transfer in the queue panel.</summary>
    public class JobVM : Notifier
    {
        string _name, _kind, _status, _right, _error, _state;
        double _percent;
        Brush _bar = Brushes2.Accent;
        int _sort;

        public int Id { get; }
        public string Name { get { return _name; } private set { Set(ref _name, value); } }
        public string KindText { get { return _kind; } private set { Set(ref _kind, value); } }
        public string Status { get { return _status; } private set { Set(ref _status, value); } }
        public string Right { get { return _right; } private set { Set(ref _right, value); } }
        public string Error { get { return _error; } private set { Set(ref _error, value); Raise("HasError"); } }
        public bool HasError { get { return !string.IsNullOrEmpty(_error); } }
        public double Percent { get { return _percent; } private set { Set(ref _percent, value); } }
        public Brush Bar { get { return _bar; } private set { Set(ref _bar, value); } }
        public int SortKey { get { return _sort; } private set { Set(ref _sort, value); } }
        public string State { get { return _state; } private set { Set(ref _state, value); Raise("CanPause"); Raise("CanResume"); Raise("CanCancel"); Raise("CanRetry"); Raise("CanRemove"); } }
        public bool CanPause { get { return _state == JobState.Downloading; } }
        public bool CanResume { get { return _state == JobState.Paused; } }
        public bool CanCancel { get { return _state == JobState.Queued || _state == JobState.Sending || _state == JobState.Downloading || _state == JobState.Paused; } }
        public bool CanRetry { get { return _state == JobState.Error || _state == JobState.Canceled; } }
        public bool CanRemove { get { return _state == JobState.Done || _state == JobState.Error || _state == JobState.Canceled; } }

        public JobVM(int id) { Id = id; }

        public void Update(Job j)
        {
            Name = j.Name;
            KindText = j.Kind == Kinds.Base ? "GAME" : j.Kind == Kinds.Update ? "UPDATE " + j.Version : "DLC";
            State = j.State;
            Error = j.Error;
            Percent = j.State == JobState.Done ? 100 : j.Percent;

            string right = "";
            switch (j.State)
            {
                case JobState.Downloading:
                    Status = j.Percent + "% · " + Fmt.Size(j.Transferred) + " / " + Fmt.Size(j.Total);
                    right = string.Join(" · ", new[] { j.Speed > 0 ? Fmt.Rate(j.Speed) : j.Phase, Fmt.Eta(j.Eta) }.Where(s => !string.IsNullOrEmpty(s)));
                    break;
                case JobState.Paused:
                    Status = j.Percent + "% · " + Fmt.Size(j.Transferred) + " / " + Fmt.Size(j.Total); right = "Paused";
                    break;
                case JobState.Sending: Status = string.IsNullOrEmpty(j.Phase) ? "Sending" : j.Phase; break;
                case JobState.Queued: Status = j.Dep != 0 ? "Queued · after the game" : "Queued"; break;
                case JobState.Done: Status = "Done · " + Fmt.Size(j.Size); break;
                case JobState.Canceled: Status = "Canceled"; break;
                default: Status = "Failed"; break;
            }
            Right = right;

            Bar = j.State == JobState.Done ? Brushes2.Ok : j.State == JobState.Error ? Brushes2.Err : j.State == JobState.Paused ? Brushes2.Warn : Brushes2.Accent;
            SortKey = j.State == JobState.Downloading || j.State == JobState.Sending ? 0 : j.State == JobState.Paused ? 1 : j.State == JobState.Queued ? 2
                    : j.State == JobState.Error ? 3 : j.State == JobState.Done ? 4 : 5;
        }
    }
}
