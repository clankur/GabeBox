using System;
using System.Collections.Generic;
using System.Threading.Tasks;

// A stand-in for a game assembly: a hot path with async state machines, a closure,
// value-type collections and both kinds of type initializer.
namespace Fixture.Combat
{
    public struct Damage
    {
        public Damage(int amount) => Amount = amount;
        public int Amount { get; }
    }

    // Precise initialization (explicit static constructor).
    public static class Precise
    {
        public static int Value;

        static Precise()
        {
            Environment.SetEnvironmentVariable("FIXTURE_PRECISE_CCTOR", "1");
            Value = 3;
        }
    }

    // beforefieldinit (field initializer only).
    public static class Relaxed
    {
        public static readonly int Value = Mark();

        static int Mark()
        {
            Environment.SetEnvironmentVariable("FIXTURE_RELAXED_CCTOR", "1");
            return 4;
        }
    }

    public sealed class Table
    {
        public static int PlayCard()
        {
            var damage = new List<Damage> { new Damage(Precise.Value + Relaxed.Value) };
            var byTarget = new Dictionary<int, Damage> { { 1, damage[0] } };
            return new Table().ResolveAsync(byTarget).GetAwaiter().GetResult() + Doubled(3);
        }

        async Task<int> ResolveAsync(Dictionary<int, Damage> byTarget)
        {
            await Task.Yield();
            return await SumAsync(byTarget.Values);
        }

        static async Task<int> SumAsync(IEnumerable<Damage> values)
        {
            await Task.Delay(1);
            var total = 0;
            foreach (var value in values)
                total += value.Amount;
            return total;
        }

        static int Doubled(int value)
        {
            Func<int, int> twice = x => x * 2;
            return twice(value);
        }
    }
}

namespace Fixture.Host
{
    // hostfxr ComponentEntryPoint, as a native host such as Godot calls into the game assembly.
    public static class Entry
    {
        public static int Play(IntPtr args, int sizeBytes) => Combat.Table.PlayCard();
    }
}

namespace Fixture.Menu
{
    public static class Title
    {
        public static int Open() => Environment.ProcessorCount > 0 ? 1 : 0;
    }
}
