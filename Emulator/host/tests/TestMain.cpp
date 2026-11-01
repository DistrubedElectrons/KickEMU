// Test runner: runs every registered juce::UnitTest, or one category given on
// the command line. Exit code is non-zero if anything failed.

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);

    if (argc > 1)
        runner.runTestsInCategory (argv[1]);
    else
        runner.runAllTests();

    int failures = 0, passes = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        auto* r = runner.getResult (i);
        failures += r->failures;
        passes += r->passes;
    }

    std::printf ("\n%d checks passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
