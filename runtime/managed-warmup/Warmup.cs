using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Text;
using System.Threading;

// First-use JIT warm-up for CoreCLR titles.
//
// The game's IL-only assemblies are compiled on first call, so the first card play, the
// first save and other first uses compile new code on the game thread. This component
// prepares those methods on a background thread soon after the assembly loads, through
// RuntimeHelpers.PrepareMethod. PrepareMethod takes the same prestub path as a first call
// (MethodDesc::DoPrestub), so each method gets the code its first call would have got:
// tier-0 code with call counting under tiered compilation. In .NET 9 JIT compilation
// never runs a class constructor (CEEInfo::initClass returns CORINFO_INITCLASS_USE_HELPER
// instead of running .cctors), so preparing a method does not run game code. The
// publisher's files are not changed.
//
// Two entries start it. Native hosts such as Godot .NET never execute an entry assembly's
// Main, so DOTNET_STARTUP_HOOKS never runs for them; the Win32 layer calls Entry.Start
// through hostfxr's load_assembly_and_get_function_pointer instead. Titles started through
// apphost or the dotnet muxer run StartupHook.Initialize.

namespace XboxManagedWarmup
{
    /// <summary>Native-host entry with hostfxr's default ComponentEntryPoint signature.</summary>
    public static class Entry
    {
        public static int Start(IntPtr args, int sizeBytes) => Warmup.Begin("hostfxr") ? 0 : 1;
    }

    internal sealed class Target
    {
        public Target(string name, Guid? mvid)
        {
            Name = name;
            Mvid = mvid;
        }

        public string Name { get; }
        public Guid? Mvid { get; }
    }

    /// <summary>The profile's warm-up plan. build_tools/stage_profiles.py applies the same rules.</summary>
    internal sealed class Plan
    {
        public const int MaxDepth = 8;
        public List<Target> Assemblies { get; } = new List<Target>();
        public List<string> Priorities { get; } = new List<string>();
        public bool Remaining { get; private set; } = true;
        public int Depth { get; private set; } = 4;

        public Target? Find(string? name)
        {
            foreach (var target in Assemblies)
                if (string.Equals(target.Name, name, StringComparison.OrdinalIgnoreCase))
                    return target;
            return null;
        }

        public static Plan? Parse(string? text, out string error)
        {
            error = "empty";
            if (string.IsNullOrEmpty(text) || text!.Length > 2048)
                return null;
            var plan = new Plan();
            bool remainingSet = false, depthSet = false;
            foreach (var raw in text.Split(';', '\n'))
            {
                var line = raw.Trim('\r', ' ');
                if (line.Length == 0 || line[0] == '#')
                    continue;
                error = "invalid-line";
                var split = line.IndexOf('=');
                if (split <= 0)
                    return null;
                var key = line.Substring(0, split);
                var value = line.Substring(split + 1);
                switch (key)
                {
                    case "assembly":
                        var parts = value.Split(' ');
                        Guid? mvid = null;
                        if (parts.Length > 2 || !IsAssemblyName(parts[0]) || plan.Find(parts[0]) != null)
                            return null;
                        if (parts.Length == 2)
                        {
                            if (!parts[1].StartsWith("mvid=", StringComparison.Ordinal) ||
                                    !Guid.TryParseExact(parts[1].Substring(5), "D", out var parsed) ||
                                    parts[1].Substring(5) != parsed.ToString("D"))
                                return null;
                            mvid = parsed;
                        }
                        if (plan.Assemblies.Count == 8)
                            return null;
                        plan.Assemblies.Add(new Target(parts[0], mvid));
                        break;
                    case "priority":
                        if (!IsPrefix(value) || plan.Priorities.Count == 32)
                            return null;
                        plan.Priorities.Add(value);
                        break;
                    case "remaining":
                        if (remainingSet || (value != "yes" && value != "no"))
                            return null;
                        plan.Remaining = value == "yes";
                        remainingSet = true;
                        break;
                    case "depth":
                        if (depthSet || value.Length != 1 || value[0] < '0' || value[0] > '0' + MaxDepth)
                            return null;
                        plan.Depth = value[0] - '0';
                        depthSet = true;
                        break;
                    default:
                        return null;
                }
            }
            error = "no-assembly";
            return plan.Assemblies.Count == 0 ? null : plan;
        }

        static bool IsAssemblyName(string value)
        {
            if (value.Length == 0 || value.Length > 64 || !(IsAsciiLetterOrDigit(value[0]) || value[0] == '_'))
                return false;
            foreach (var c in value)
                if (!(IsAsciiLetterOrDigit(c) || c == '_' || c == '.' || c == '-'))
                    return false;
            return true;
        }

        static bool IsPrefix(string value)
        {
            if (value.Length == 0 || value.Length > 160 || !(IsAsciiLetter(value[0]) || value[0] == '_'))
                return false;
            foreach (var c in value)
                if (!(IsAsciiLetterOrDigit(c) || c == '_' || c == '.' || c == '+' || c == '`'))
                    return false;
            return true;
        }

        static bool IsAsciiLetter(char c) => (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        static bool IsAsciiLetterOrDigit(char c) => IsAsciiLetter(c) || (c >= '0' && c <= '9');
    }

    internal enum ImageKind { IlOnly, ReadyToRun, Other, Unreadable }

    /// <summary>Reads the CLI header: only IL-only images are prepared directly.</summary>
    internal static class PeImage
    {
        public static ImageKind Classify(string? path)
        {
            if (string.IsNullOrEmpty(path))
                return ImageKind.Unreadable;
            try
            {
                using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                var header = new byte[4096];
                var length = file.Read(header, 0, header.Length);
                if (length < 64 || header[0] != (byte)'M' || header[1] != (byte)'Z')
                    return ImageKind.Unreadable;
                var pe = BitConverter.ToInt32(header, 0x3c);
                if (pe < 64 || pe + 24 > length || BitConverter.ToUInt32(header, pe) != 0x00004550)
                    return ImageKind.Unreadable;
                int sections = BitConverter.ToUInt16(header, pe + 6);
                int optionalSize = BitConverter.ToUInt16(header, pe + 20);
                var optional = pe + 24;
                var magic = BitConverter.ToUInt16(header, optional);
                var directories = magic == 0x10b ? 96 : magic == 0x20b ? 112 : -1;
                var table = optional + optionalSize;
                if (directories < 0 || optionalSize < directories + 15 * 8 || table + sections * 40 > length)
                    return ImageKind.Unreadable;
                var clrRva = BitConverter.ToUInt32(header, optional + directories + 14 * 8);
                if (clrRva == 0)
                    return ImageKind.Other;
                for (var section = 0; section < sections; section++)
                {
                    var entry = table + section * 40;
                    var address = BitConverter.ToUInt32(header, entry + 12);
                    var rawSize = BitConverter.ToUInt32(header, entry + 16);
                    var rawOffset = BitConverter.ToUInt32(header, entry + 20);
                    if (clrRva < address || clrRva - address + 72 > rawSize)
                        continue;
                    var cli = new byte[72];
                    file.Position = rawOffset + (clrRva - address);
                    if (file.Read(cli, 0, cli.Length) != cli.Length || BitConverter.ToUInt32(cli, 0) < 72)
                        return ImageKind.Unreadable;
                    var flags = BitConverter.ToUInt32(cli, 16);
                    var nativeHeader = BitConverter.ToUInt32(cli, 64);
                    if (nativeHeader != 0)
                        return ImageKind.ReadyToRun;
                    return (flags & 1) != 0 ? ImageKind.IlOnly : ImageKind.Other;
                }
                return ImageKind.Unreadable;
            }
            catch (Exception)
            {
                return ImageKind.Unreadable;
            }
        }
    }

    internal static class Warmup
    {
        const string ActiveVariable = "XBOX_WIN32_MANAGED_WARMUP_ACTIVE";
        static readonly object Gate = new object();
        static readonly HashSet<Assembly> Offered = new HashSet<Assembly>();
        static readonly BlockingCollection<Assembly> Queue = new BlockingCollection<Assembly>();
        static Plan? plan;
        static string? logPath;

        public static bool Begin(string via)
        {
            try
            {
                lock (Gate)
                {
                    // One warm-up per process, whichever entry runs first.
                    if (Environment.GetEnvironmentVariable(ActiveVariable) == "1")
                        return false;
                    Environment.SetEnvironmentVariable(ActiveVariable, "1");
                    logPath = Environment.GetEnvironmentVariable("XBOX_WIN32_MANAGED_WARMUP_LOG");
                    plan = Plan.Parse(Environment.GetEnvironmentVariable("XBOX_WIN32_MANAGED_WARMUP_PLAN"), out var error);
                    if (plan == null)
                    {
                        Log("managed_warmup=plan-error reason=" + error);
                        return false;
                    }
                    Log("managed_warmup=entered via=" + via + " runtime=" + Environment.Version +
                        " processors=" + Environment.ProcessorCount + " depth=" + plan.Depth +
                        " remaining=" + (plan.Remaining ? "yes" : "no") + " utc=" + Now());
                }
                var domain = AppDomain.CurrentDomain;
                domain.AssemblyLoad += (_, loaded) => Offer(loaded.LoadedAssembly);
                foreach (var assembly in domain.GetAssemblies())
                    Offer(assembly);
                new Thread(Run) { IsBackground = true, Name = "XboxManagedWarmup", Priority = ThreadPriority.BelowNormal }.Start();
                return true;
            }
            catch (Exception exception)
            {
                Log("managed_warmup=start-error exception=" + exception.GetType().Name);
                return false;
            }
        }

        static void Offer(Assembly assembly)
        {
            try
            {
                if (assembly.IsDynamic || plan?.Find(assembly.GetName().Name) == null)
                    return;
                lock (Gate)
                    if (!Offered.Add(assembly))
                        return;
                Queue.Add(assembly);
            }
            catch (Exception)
            {
            }
        }

        static void Run()
        {
            try
            {
                var active = plan!;
                var done = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var clock = Stopwatch.StartNew();
                var limit = TimeSpan.FromMinutes(10);
                while (done.Count < active.Assemblies.Count)
                {
                    var wait = limit - clock.Elapsed;
                    if (wait <= TimeSpan.Zero || !Queue.TryTake(out var assembly, wait))
                    {
                        Log("managed_warmup=timeout waiting=" + Missing(active, done) + " utc=" + Now());
                        return;
                    }
                    done.Add(assembly.GetName().Name!);
                    new AssemblyWarmup(active, assembly).Run();
                }
                Log("managed_warmup=complete utc=" + Now());
            }
            catch (Exception exception)
            {
                Log("managed_warmup=error exception=" + exception.GetType().Name);
            }
        }

        static string Missing(Plan active, HashSet<string> done)
        {
            var names = new List<string>();
            foreach (var target in active.Assemblies)
                if (!done.Contains(target.Name))
                    names.Add(target.Name);
            return string.Join(",", names);
        }

        public static string Now() => DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture);

        public static void Log(string line)
        {
            var path = logPath;
            if (string.IsNullOrEmpty(path))
                return;
            try
            {
                lock (Gate)
                    File.AppendAllText(path, line + "\n");
            }
            catch (Exception)
            {
            }
        }
    }

    internal sealed class AssemblyWarmup
    {
        const BindingFlags Declared = BindingFlags.DeclaredOnly | BindingFlags.Public | BindingFlags.NonPublic |
            BindingFlags.Instance | BindingFlags.Static;
        static readonly OpCode[] OneByte = new OpCode[0x100];
        static readonly OpCode[] TwoByte = new OpCode[0x100];

        readonly Plan plan;
        readonly Assembly assembly;
        readonly Module module;
        readonly HashSet<(IntPtr, IntPtr)> seen = new HashSet<(IntPtr, IntPtr)>();
        readonly HashSet<IntPtr> roots = new HashSet<IntPtr>();
        readonly HashSet<IntPtr> allocated = new HashSet<IntPtr>();
        readonly Dictionary<Module, bool> ilOnly = new Dictionary<Module, bool>();
        readonly Dictionary<string, int> failures = new Dictionary<string, int>(StringComparer.Ordinal);
        int prepared, instantiations, callees, failed;

        static AssemblyWarmup()
        {
            foreach (var field in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
            {
                if (!(field.GetValue(null) is OpCode code))
                    continue;
                var value = (ushort)code.Value;
                if (code.Size == 1)
                    OneByte[value] = code;
                else if ((value >> 8) == 0xfe)
                    TwoByte[value & 0xff] = code;
            }
        }

        public AssemblyWarmup(Plan plan, Assembly assembly)
        {
            this.plan = plan;
            this.assembly = assembly;
            module = assembly.ManifestModule;
        }

        public void Run()
        {
            var name = assembly.GetName().Name;
            var target = plan.Find(name)!;
            var mvid = module.ModuleVersionId;
            var label = "managed_warmup_assembly=" + name + " mvid=" + mvid.ToString("D");
            if (target.Mvid.HasValue && target.Mvid.Value != mvid)
            {
                Warmup.Log(label + " result=mvid-mismatch expected=" + target.Mvid.Value.ToString("D"));
                return;
            }
            var kind = PeImage.Classify(assembly.Location);
            if (kind != ImageKind.IlOnly)
            {
                Warmup.Log(label + " result=skipped image=" + kind);
                return;
            }
            ilOnly[module] = true;
            var clock = Stopwatch.StartNew();
            var groups = CollectRoots();
            var total = 0;
            foreach (var group in groups)
                total += group.Count;
            Warmup.Log(label + " image=IlOnly roots=" + total + " utc=" + Warmup.Now());
            for (var rank = 0; rank < groups.Length; rank++)
            {
                var before = prepared + instantiations + callees;
                var groupClock = Stopwatch.StartNew();
                foreach (var method in groups[rank])
                {
                    if (Prepare(method))
                        prepared++;
                    Scan(method, 1);
                }
                if (groups[rank].Count == 0)
                    continue;
                var prefix = rank < plan.Priorities.Count ? plan.Priorities[rank] : "*";
                Warmup.Log("managed_warmup_group=" + prefix + " roots=" + groups[rank].Count + " prepared=" +
                           (prepared + instantiations + callees - before) + " ms=" + groupClock.ElapsedMilliseconds +
                           " utc=" + Warmup.Now());
            }
            var summary = new StringBuilder(label)
                .Append(" result=prepared roots=").Append(prepared)
                .Append(" instantiations=").Append(instantiations)
                .Append(" callees=").Append(callees)
                .Append(" failed=").Append(failed)
                .Append(" ms=").Append(clock.ElapsedMilliseconds)
                .Append(" utc=").Append(Warmup.Now());
            foreach (var failure in failures)
                summary.Append(" failure.").Append(failure.Key).Append('=').Append(failure.Value);
            Warmup.Log(summary.ToString());
        }

        List<MethodBase>[] CollectRoots()
        {
            var groups = new List<MethodBase>[plan.Priorities.Count + 1];
            for (var index = 0; index < groups.Length; index++)
                groups[index] = new List<MethodBase>();
            Type?[] types;
            try
            {
                types = module.GetTypes();
            }
            catch (ReflectionTypeLoadException exception)
            {
                types = exception.Types;
                Fail(exception);
            }
            foreach (var type in types)
            {
                if (type == null || type.ContainsGenericParameters || type.IsInterface)
                    continue;
                var rank = Rank(type.FullName ?? type.Name);
                if (rank < 0)
                    continue;
                try
                {
                    foreach (var method in type.GetMethods(Declared))
                        if (IsRoot(method))
                            groups[rank].Add(method);
                    // Includes the type initializer: compiling it does not run it.
                    foreach (var constructor in type.GetConstructors(Declared))
                        if (IsRoot(constructor))
                            groups[rank].Add(constructor);
                }
                catch (Exception exception)
                {
                    Fail(exception);
                }
            }
            foreach (var group in groups)
                foreach (var method in group)
                    roots.Add(method.MethodHandle.Value);
            return groups;
        }

        int Rank(string fullName)
        {
            for (var index = 0; index < plan.Priorities.Count; index++)
            {
                var prefix = plan.Priorities[index];
                if (fullName.StartsWith(prefix, StringComparison.Ordinal) &&
                        (fullName.Length == prefix.Length || fullName[prefix.Length] == '.' || fullName[prefix.Length] == '+'))
                    return index;
            }
            return plan.Remaining ? plan.Priorities.Count : -1;
        }

        static bool IsRoot(MethodBase method) =>
            !method.IsAbstract && !method.ContainsGenericParameters && HasIlBody(method);

        static bool HasIlBody(MethodBase method)
        {
            var implementation = method.MethodImplementationFlags;
            return (implementation & MethodImplAttributes.CodeTypeMask) == MethodImplAttributes.IL &&
                (implementation & MethodImplAttributes.InternalCall) == 0 &&
                (method.Attributes & MethodAttributes.PinvokeImpl) == 0;
        }

        bool Prepare(MethodBase method)
        {
            try
            {
                var type = method.DeclaringType!;
                if (!type.IsGenericType && !method.IsGenericMethod)
                {
                    RuntimeHelpers.PrepareMethod(method.MethodHandle);
                    return true;
                }
                var arguments = new List<RuntimeTypeHandle>();
                if (type.IsGenericType)
                    foreach (var argument in type.GetGenericArguments())
                        arguments.Add(argument.TypeHandle);
                if (method.IsGenericMethod)
                    foreach (var argument in method.GetGenericArguments())
                        arguments.Add(argument.TypeHandle);
                RuntimeHelpers.PrepareMethod(method.MethodHandle, arguments.ToArray());
                return true;
            }
            catch (Exception exception)
            {
                Fail(exception);
                return false;
            }
        }

        // Follows the call graph from prepared code to the generic instantiations it will
        // need (async builders and state-machine boxes over the game's state machines,
        // collections over its value types) and to non-generic callees in other IL-only
        // assemblies, such as GodotSharp. Calls into ReadyToRun assemblies stop here.
        void Scan(MethodBase method, int depth)
        {
            if (depth > plan.Depth)
                return;
            byte[]? il;
            Type[]? typeArguments;
            Type[]? methodArguments;
            try
            {
                il = method.GetMethodBody()?.GetILAsByteArray();
                typeArguments = method.DeclaringType != null && method.DeclaringType.IsGenericType
                    ? method.DeclaringType.GetGenericArguments() : null;
                methodArguments = method.IsGenericMethod ? method.GetGenericArguments() : null;
            }
            catch (Exception exception)
            {
                Fail(exception);
                return;
            }
            if (il == null)
                return;
            var offset = 0;
            while (offset < il.Length)
            {
                OpCode code;
                if (il[offset] == 0xfe)
                {
                    if (offset + 1 >= il.Length)
                        return;
                    code = TwoByte[il[offset + 1]];
                    offset += 2;
                }
                else
                {
                    code = OneByte[il[offset]];
                    offset += 1;
                }
                if (code.Size == 0)
                    return;
                var operand = OperandSize(code.OperandType, il, offset);
                if (operand < 0 || offset + operand > il.Length)
                    return;
                if (code.OperandType == OperandType.InlineMethod)
                {
                    MethodBase? callee = null;
                    try
                    {
                        callee = method.Module.ResolveMethod(BitConverter.ToInt32(il, offset), typeArguments, methodArguments);
                    }
                    catch (Exception)
                    {
                    }
                    if (callee != null)
                        Consider(callee, code == OpCodes.Newobj, depth, method.DeclaringType);
                }
                offset += operand;
            }
        }

        // A callee found at a given depth is prepared, and its own callees are found one level
        // deeper. Calls between members of one closed type, and the members an allocation
        // implies (type initializer and overrides of a value-type instantiation), stay at the
        // caller's depth: they are a small fixed set per type, such as a state-machine box's
        // MoveNext overloads or a collection's enumerator.
        void Consider(MethodBase callee, bool allocates, int depth, Type? caller)
        {
            try
            {
                var type = callee.DeclaringType;
                if (type == null || callee.IsAbstract || callee.ContainsGenericParameters || !HasIlBody(callee))
                    return;
                var next = type == caller ? depth : depth + 1;
                if (type.IsGenericType || callee.IsGenericMethod)
                {
                    // Instantiations over reference types share canonical code, which is
                    // compiled once (or precompiled); only value-type instantiations, or
                    // canonical code from an IL-only assembly, need a JIT of their own.
                    var valueArgument = HasValueType(type.IsGenericType ? type.GetGenericArguments() : null) ||
                        HasValueType(callee.IsGenericMethod ? callee.GetGenericArguments() : null);
                    if (!valueArgument && !IsIlOnly(callee.Module))
                        return;
                    if (seen.Add((callee.MethodHandle.Value, type.TypeHandle.Value)))
                    {
                        if (Prepare(callee))
                            instantiations++;
                        Scan(callee, next);
                    }
                    // A constructor can be reached first through a derived type's constructor,
                    // so the members an allocation implies are tracked per type.
                    if (allocates && valueArgument && allocated.Add(type.TypeHandle.Value))
                    {
                        if (type.TypeInitializer != null)
                            Consider(type.TypeInitializer, false, next, type);
                        foreach (var method in type.GetMethods(BindingFlags.DeclaredOnly | BindingFlags.Public |
                                                               BindingFlags.NonPublic | BindingFlags.Instance))
                            if (method.IsVirtual)
                                Consider(method, false, next, type);
                    }
                    return;
                }
                if (roots.Contains(callee.MethodHandle.Value) || !IsIlOnly(callee.Module) ||
                        !seen.Add((callee.MethodHandle.Value, IntPtr.Zero)))
                    return;
                if (Prepare(callee))
                    callees++;
                Scan(callee, next);
            }
            catch (Exception exception)
            {
                Fail(exception);
            }
        }

        static bool HasValueType(Type[]? arguments)
        {
            if (arguments != null)
                foreach (var argument in arguments)
                    if (argument.IsValueType)
                        return true;
            return false;
        }

        bool IsIlOnly(Module candidate)
        {
            if (!ilOnly.TryGetValue(candidate, out var result))
            {
                string? location = null;
                try
                {
                    location = candidate.Assembly.IsDynamic ? null : candidate.Assembly.Location;
                }
                catch (Exception)
                {
                }
                result = PeImage.Classify(location) == ImageKind.IlOnly;
                ilOnly[candidate] = result;
            }
            return result;
        }

        static int OperandSize(OperandType type, byte[] il, int offset)
        {
            switch (type)
            {
                case OperandType.InlineNone:
                    return 0;
                case OperandType.ShortInlineBrTarget:
                case OperandType.ShortInlineI:
                case OperandType.ShortInlineVar:
                    return 1;
                case OperandType.InlineVar:
                    return 2;
                case OperandType.InlineI8:
                case OperandType.InlineR:
                    return 8;
                case OperandType.InlineSwitch:
                    if (offset + 4 > il.Length)
                        return -1;
                    var count = BitConverter.ToUInt32(il, offset);
                    return count > (uint)(il.Length / 4) ? -1 : 4 + (int)count * 4;
                default:
                    return 4;
            }
        }

        void Fail(Exception exception)
        {
            failed++;
            var name = exception.GetType().Name;
            failures.TryGetValue(name, out var count);
            failures[name] = count + 1;
        }
    }
}

/// <summary>DOTNET_STARTUP_HOOKS entry for titles started through apphost or the dotnet muxer.</summary>
internal static class StartupHook
{
    public static void Initialize() => XboxManagedWarmup.Warmup.Begin("startup-hook");
}
