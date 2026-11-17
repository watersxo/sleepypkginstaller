using System;
using System.IO;
using System.Threading;
using System.Windows;

namespace Sleepy
{
    public partial class App : Application
    {
        public const string WindowTitle = "Sleepy Package Installer";
        public const string ClientVersion = "1.6.0";
        Mutex _single;
        FileServer _fs;
        Manager _mgr;

        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);

            bool first;
            _single = new Mutex(true, @"Local\SleepyPackageInstallerClient", out first);
            if (!first)
            {
                Native.ActivateExisting(WindowTitle);
                Shutdown();
                return;
            }

            DispatcherUnhandledException += (s, a) =>
            {
                AppLog.Write("UNHANDLED UI EXCEPTION: " + a.Exception);
                MessageBox.Show("Something went wrong:\n\n" + a.Exception.Message + "\n\nDetails are in " + AppLog.FilePath, WindowTitle, MessageBoxButton.OK, MessageBoxImage.Warning);
                a.Handled = true;
            };
            AppDomain.CurrentDomain.UnhandledException += (s, a) => AppLog.Write("UNHANDLED EXCEPTION (terminating=" + a.IsTerminating + "): " + a.ExceptionObject);
            System.Threading.Tasks.TaskScheduler.UnobservedTaskException += (s, a) => { AppLog.Write("UNOBSERVED TASK EXCEPTION: " + a.Exception); a.SetObserved(); };
            AppLog.Write("==== Sleepy Package Installer client " + ClientVersion + " started ====");

            var cfg = new ConfigStore();
            var lib = new Library();
            _fs = new FileServer(lib);
            try { _fs.Start(cfg.Get().FilePort); }
            catch (Exception ex)
            {
                MessageBox.Show("The package server could not start:\n\n" + ex.Message, WindowTitle, MessageBoxButton.OK, MessageBoxImage.Error);
                Shutdown();
                return;
            }
            _mgr = new Manager(cfg, lib, _fs);
            _mgr.Start();

            new MainWindow(cfg, lib, _fs, _mgr).Show();
        }

        protected override void OnExit(ExitEventArgs e)
        {
            AppLog.Write("client exiting");
            _mgr?.Stop();
            _fs?.Stop();
            base.OnExit(e);
        }
    }
}
