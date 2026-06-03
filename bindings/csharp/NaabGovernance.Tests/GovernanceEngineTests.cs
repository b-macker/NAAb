using Xunit;
using NaabGovernance;

namespace NaabGovernance.Tests;

public class GovernanceEngineTests
{
    private const string TestConfig =
        @"{""version"":""3.0"",""mode"":""enforce"",
          ""restrictions"":{""dangerous_calls"":{""level"":""hard""},
          ""code_injection"":{""level"":""hard""}},
          ""code_quality"":{""no_secrets"":{""level"":""hard""}}}";

    [Fact]
    public void Version_ReturnsNonEmpty()
    {
        var v = GovernanceEngine.Version();
        Assert.False(string.IsNullOrEmpty(v));
    }

    [Fact]
    public void Lifecycle_CreateAndDispose()
    {
        using var engine = new GovernanceEngine();
        Assert.NotNull(engine);
    }

    [Fact]
    public void Lifecycle_DoubleDispose()
    {
        var engine = new GovernanceEngine();
        engine.Dispose();
        engine.Dispose(); // should not throw
    }

    [Fact]
    public void ScanSafeCode_NotBlocked()
    {
        using var engine = new GovernanceEngine();
        engine.LoadConfigString(TestConfig);
        Assert.True(engine.IsActive);

        var result = engine.Scan("python", "x = 42\nprint(x)", "test.py", 1);
        Assert.NotNull(result);
        Assert.False(engine.WasBlocked);
    }

    [Fact]
    public void ScanDangerousCode_Blocked()
    {
        using var engine = new GovernanceEngine();
        engine.LoadConfigString(TestConfig);

        engine.Scan("python", "import os; os.system('rm -rf /')", "test.py", 1);
        Assert.True(engine.WasBlocked);
    }

    [Fact]
    public void Reset_ClearsResults()
    {
        using var engine = new GovernanceEngine();
        engine.LoadConfigString(TestConfig);

        engine.Scan("python", "eval(input())", "test.py", 1);
        Assert.True(engine.ResultCount > 0);

        engine.Reset();
        Assert.Equal(0, engine.ResultCount);
    }

    [Fact]
    public void IsActive_FalseBeforeConfig()
    {
        using var engine = new GovernanceEngine();
        Assert.False(engine.IsActive);
    }
}
