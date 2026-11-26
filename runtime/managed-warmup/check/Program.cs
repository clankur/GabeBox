using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Tracing;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using System.Threading;

// Desktop check for XboxManagedWarmup.
//
//   WarmupCheck XboxManagedWarmup.dll WarmupFixture.dll
//
// runs three child processes against the fixture game assembly, each loaded into an
// isolated AssemblyLoadContext the way hostfxr's load_assembly_and_get_function_pointer
// loads a Godot game assembly:
//   control  no warm-up; the fixture's hot path must JIT on first use
//   warm     Entry.Start called from the component's own isolated context (the native
//            Win32 bootstrap's path); the hot path must not JIT and no type initializer
//            may run before the game calls it
//   hook     the same through DOTNET_STARTUP_HOOKS
//   collect  the warm path with collect=after; the log must record one forced gen2
//            collection after the warm-up completes
// and also checks that a mismatched MVID pin refuses the assembly.
static class Program
{
    const string Plan = "assembly=WarmupFixture;priority=Fixture.Combat;remaining=no;depth=4";

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: WarmupCheck XboxManagedWarmup.dll WarmupFixture.dll [mode]");
            return 2;
        }
        var warmup = Path.GetFullPath(args[0]);
        var fixture = Path.GetFullPath(args[1]);
        if (args.Length > 2)
            return RunMode(args[2], warmup, fixture);

        var failures = 0;
        var log = Path.Combine(Path.GetTempPath(), "xbox-managed-warmup-check-" + Environment.ProcessId);
        var control = Child("control", warmup, fixture, null, null);
        var warm = Child("warm", warmup, fixture, log + "-warm.log", null);
        var hook = Child("hook", warmup, fixture, log + "-hook.log", warmup);
        var pinned = Child("pinned", warmup, fixture, log + "-pinned.log", null);
        var collect = Child("collect", warmup, fixture, log + "-collect.log", null);
        var collectLog = File.Exists(log + "-collect.log") ? File.ReadAllText(log + "-collect.log") : "";
        failures += Expect(control.Code == 0 && control.Jitted > 0, $"control: fixture JIT on first use = {control.Jitted}");
        failures += Expect(warm.Code == 0 && warm.Jitted == 0, $"warm: fixture JIT on first use = {warm.Jitted}");
        failures += Expect(hook.Code == 0 && hook.Jitted == 0, $"hook: fixture JIT on first use = {hook.Jitted}");
        failures += Expect(pinned.Code == 0 && pinned.Jitted > 0, $"pinned to another MVID: fixture JIT on first use = {pinned.Jitted}");
        failures += Expect(collect.Code == 0 && collect.Jitted == 0, $"collect=after: fixture JIT on first use = {collect.Jitted}");
        failures += Expect(CollectedAfterComplete(collectLog), "collect=after: one forced gen2 collection logged after completion");
        failures += Expect(!File.ReadAllText(log + "-warm.log").Contains("managed_warmup_collect"),
            "default plan: no forced collection");
        foreach (var path in new[] { log + "-warm.log", log + "-hook.log", log + "-pinned.log", log + "-collect.log" })
        {
            if (!File.Exists(path))
                continue;
            Console.WriteLine("--- " + Path.GetFileName(path));
            Console.Write(File.ReadAllText(path));
            File.Delete(path);
        }
        Console.WriteLine(failures == 0 ? "managed_warmup_check=passed" : "managed_warmup_check=failed");
        return failures == 0 ? 0 : 1;
    }

    static bool CollectedAfterComplete(string log)
    {
        var complete = log.IndexOf("managed_warmup=complete", StringComparison.Ordinal);
        var collect = log.IndexOf("managed_warmup_collect=after generation=2 mode=forced blocking=yes compacting=yes ms=",
            StringComparison.Ordinal);
        return complete >= 0 && collect > complete && log.IndexOf("managed_warmup_collect", collect + 1, StringComparison.Ordinal) < 0 &&
            log.IndexOf(" heap_kib_after=", collect, StringComparison.Ordinal) > collect;
    }

    static int Expect(bool condition, string description)
    {
        Console.WriteLine((condition ? "ok   " : "FAIL ") + description);
        return condition ? 0 : 1;
    }

    static (int Code, int Jitted) Child(string mode, string warmup, string fixture, string? log, string? hook)
    {
        var self = Environment.ProcessPath!;
        var start = new ProcessStartInfo(self) { RedirectStandardOutput = true, UseShellExecute = false };
        if (Path.GetFileNameWithoutExtension(self) == "dotnet")
            start.ArgumentList.Add(typeof(Program).Assembly.Location);
        foreach (var argument in new[] { warmup, fixture, mode })
            start.ArgumentList.Add(argument);
        start.Environment.Remove("DOTNET_STARTUP_HOOKS");
        start.Environment.Remove("XBOX_WIN32_MANAGED_WARMUP_ACTIVE");
        if (log != null)
        {
            start.Environment["XBOX_WIN32_MANAGED_WARMUP_LOG"] = log;
            start.Environment["XBOX_WIN32_MANAGED_WARMUP_PLAN"] = mode == "collect" ? Plan + ";collect=after" : Plan;
        }
        if (hook != null)
            start.Environment["DOTNET_STARTUP_HOOKS"] = hook;
        using var child = Process.Start(start)!;
        var output = child.StandardOutput.ReadToEnd();
        child.WaitForExit();
        Console.Write(output);
        var jitted = -1;
        foreach (var line in output.Split('\n'))
            if (line.StartsWith("fixture_jit=", StringComparison.Ordinal))
                jitted = int.Parse(line.Substring("fixture_jit=".Length).Trim());
        return (child.ExitCode, jitted);
    }

    static int RunMode(string mode, string warmup, string fixture)
    {
        var game = new AssemblyLoadContext("game").LoadFromAssemblyPath(fixture);
        if (mode == "pinned")
        {
            Environment.SetEnvironmentVariable("XBOX_WIN32_MANAGED_WARMUP_PLAN",
                "assembly=WarmupFixture mvid=00000000-0000-0000-0000-000000000001;depth=4");
        }
        if (mode == "warm" || mode == "pinned" || mode == "collect")
        {
            var component = new AssemblyLoadContext("component").LoadFromAssemblyPath(warmup);
            var started = (int)component.GetType("XboxManagedWarmup.Entry", true)!
                .GetMethod("Start")!.Invoke(null, new object[] { IntPtr.Zero, 0 })!;
            Console.WriteLine("entry_start=" + started);
        }
        var log = Environment.GetEnvironmentVariable("XBOX_WIN32_MANAGED_WARMUP_LOG");
        var finished = mode == "pinned" ? "result=mvid-mismatch" : mode == "collect" ? "managed_warmup_collect=" : "managed_warmup=complete";
        if (log != null && !WaitFor(log, finished))
        {
            Console.WriteLine("warmup_wait=timeout");
            return 3;
        }
        var initialized = Environment.GetEnvironmentVariable("FIXTURE_PRECISE_CCTOR") != null ||
            Environment.GetEnvironmentVariable("FIXTURE_RELAXED_CCTOR") != null;
        Console.WriteLine("fixture_cctor_before_first_use=" + (initialized ? "ran" : "not-run"));
        if (initialized)
            return 4;

        var play = (Func<int>)Delegate.CreateDelegate(typeof(Func<int>),
            game.GetType("Fixture.Combat.Table", true)!.GetMethod("PlayCard")!);
        using var listener = new JitListener();
        Thread.Sleep(1500);
        listener.Counting = true;
        var result = play();
        Thread.Sleep(2500);
        listener.Counting = false;
        Console.WriteLine("fixture_result=" + result);
        foreach (var name in listener.Names)
            Console.WriteLine("  jit " + name);
        Console.WriteLine("fixture_jit=" + listener.Names.Count);
        return result == 13 ? 0 : 5;
    }

    static bool WaitFor(string log, string marker)
    {
        var clock = Stopwatch.StartNew();
        while (clock.Elapsed < TimeSpan.FromSeconds(60))
        {
            if (File.Exists(log) && File.ReadAllText(log).Contains(marker))
                return true;
            Thread.Sleep(100);
        }
        return false;
    }

    // Counts the runtime's MethodJittingStarted events for methods that mention the fixture,
    // including generic instantiations over its types.
    sealed class JitListener : EventListener
    {
        readonly object gate = new object();
        readonly List<string> names = new List<string>();
        public volatile bool Counting;

        public List<string> Names
        {
            get { lock (gate) return new List<string>(names); }
        }

        protected override void OnEventSourceCreated(EventSource source)
        {
            if (source.Name == "Microsoft-Windows-DotNETRuntime")
                EnableEvents(source, EventLevel.Verbose, (EventKeywords)0x10);
        }

        protected override void OnEventWritten(EventWrittenEventArgs data)
        {
            if (!Counting || data.EventName != "MethodJittingStarted_V1" || data.Payload == null || data.PayloadNames == null)
                return;
            var type = data.Payload[data.PayloadNames.IndexOf("MethodNamespace")] as string ?? "";
            var method = data.Payload[data.PayloadNames.IndexOf("MethodName")] as string ?? "";
            if (!type.Contains("Fixture"))
                return;
            lock (gate)
                names.Add(type + "::" + method);
        }
    }
}
