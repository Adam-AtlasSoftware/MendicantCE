// GOAT H1 autosplitter sample (docs/speedrun_state.md): finds the speedrun state block by its signature, then
// starts on the run's first tick, splits on each level won, pauses game time while loading, and resets when a new
// run begins. Adapt it freely.

state("halo") {}

startup
{
    // the block's offsets (port/linux/game/speedrun_state.h, version 1)
    vars.OffsetVersion = 0x10;
    vars.OffsetFlags = 0x20;
    vars.OffsetRun = 0x24;
    vars.OffsetRunTicks = 0x28;
    vars.OffsetLevelsCompleted = 0x2C;
    vars.FlagLoading = 1;
    // "GOAT H1 SPEEDRUN"
    vars.Signature = new SigScanTarget(0, "47 4F 41 54 20 48 31 20 53 50 45 45 44 52 55 4E");
}

init
{
    IntPtr block = IntPtr.Zero;

    // The game's memory is a 32-bit guest's, below 4 GB, and not a module: scan its pages.
    foreach (var page in game.MemoryPages(true))
    {
        long start = (long)page.BaseAddress;
        long size = (long)page.RegionSize;
        if (start >= 0x100000000L || size > 0x10000000L)
            continue;
        block = new SignatureScanner(game, page.BaseAddress, (int)size).Scan(vars.Signature);
        if (block != IntPtr.Zero)
            break;
    }
    // (not written yet, or the launcher copy of halo.exe: try again)
    if (block == IntPtr.Zero)
        throw new Exception("GOAT H1 speedrun state block not found yet");
    if (game.ReadValue<int>(block + (int)vars.OffsetVersion) != 1)
        throw new Exception("unknown GOAT H1 speedrun state block version");

    vars.Flags = new MemoryWatcher<uint>(block + (int)vars.OffsetFlags);
    vars.Run = new MemoryWatcher<uint>(block + (int)vars.OffsetRun);
    vars.RunTicks = new MemoryWatcher<uint>(block + (int)vars.OffsetRunTicks);
    vars.LevelsCompleted = new MemoryWatcher<uint>(block + (int)vars.OffsetLevelsCompleted);
    vars.Watchers = new MemoryWatcherList() { vars.Flags, vars.Run, vars.RunTicks, vars.LevelsCompleted };
}

update
{
    vars.Watchers.UpdateAll(game);
}

start
{
    return vars.RunTicks.Old == 0 && vars.RunTicks.Current > 0;
}

split
{
    return vars.LevelsCompleted.Current > vars.LevelsCompleted.Old;
}

reset
{
    return vars.Run.Current != vars.Run.Old;
}

isLoading
{
    return (vars.Flags.Current & vars.FlagLoading) != 0;
}

// The game's own ticks as game time (30 a second), in place of load-removed time:
// gameTime
// {
//     return TimeSpan.FromSeconds(vars.RunTicks.Current / 30.0);
// }
