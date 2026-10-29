using Pulsar.Services;
using Xunit;

// The launcher reads join codes the DLL makes (include/Network/JoinCode.h). The sample is
// the code the C++ test pins (tests/test_join_code.cpp, JoinCode.HostCodeRoundTrip).
public class JoinCodeTextTests
{
    private const string Sample = "PSR-040T1-8D2ME-JAB9N-7N2MT-NAXCN-PQAY0-R1R2M-0250Y-C41CP-03H0Z-4240Y-B01RG-E7K1J-BA0";

    [Fact]
    public void ReadsTheDllsSampleCode()
    {
        var c = JoinCodeText.Decode(Sample);
        Assert.NotNull(c);
        Assert.Equal(JoinCodeText.Kind.Host, c!.Kind);
        Assert.Equal(3, c.Endpoints.Count);
        Assert.Equal(new JoinCodeText.Endpoint(1, "192.168.1.20", 7777), c.Endpoints[0]);
        Assert.Equal(new JoinCodeText.Endpoint(2, "203.0.113.7", 51234), c.Endpoints[1]);
        Assert.Equal(new JoinCodeText.Endpoint(3, "203.0.113.7", 7777), c.Endpoints[2]);
    }

    [Fact]
    public void ForgivesCaseSpacesAndLookalikes()
    {
        var sloppy = " " + Sample.ToLowerInvariant().Replace("-", " ").Replace('0', 'o').Replace('1', 'l') + "\r\n";
        Assert.NotNull(JoinCodeText.Decode(sloppy));
        Assert.Equal(Sample, JoinCodeText.Normalize(sloppy));
    }

    [Fact]
    public void RejectsEveryOneCharacterTypo()
    {
        for (int i = 4; i < Sample.Length; i++)
        {
            if (Sample[i] == '-') continue;
            var chars = Sample.ToCharArray();
            chars[i] = chars[i] == 'X' ? 'Y' : 'X';
            Assert.Null(JoinCodeText.Decode(new string(chars)));
        }
    }

    [Theory]
    [InlineData("")]
    [InlineData("hello")]
    [InlineData("PSR-")]
    [InlineData("PSR-UUUUU")]
    [InlineData("192.168.1.20:7777")]
    public void RejectsGarbage(string text) => Assert.Null(JoinCodeText.Decode(text));
}
