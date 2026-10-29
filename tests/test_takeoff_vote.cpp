// Takeoff ready vote, pilot side (include/Coop/TakeoffVotePolicy.h).

#include "TestHarness.h"

#include "Coop/TakeoffVotePolicy.h"

using Pulsar::Coop::TakeoffVotePolicy;
using Request = TakeoffVotePolicy::Request;
using Answer  = TakeoffVotePolicy::Answer;
using Step    = TakeoffVotePolicy::Step;

TEST(TakeoffVote, SoloLaunchPasses)
{
    TakeoffVotePolicy v;
    CHECK(v.LaunchRequested(1000, 0) == Request::kPass);
    CHECK(!v.Open());
}

TEST(TakeoffVote, LaunchWaitsForTheCrew)
{
    TakeoffVotePolicy v;
    CHECK(v.LaunchRequested(1000, 1) == Request::kHoldAndAsk);
    CHECK(v.LaunchRequested(2000, 1) == Request::kHold);       // pilot asks again
    CHECK(v.Tick(30000) == Step::kNone);                       // still waiting
    CHECK(v.Ready(v.Id(), 1, 31000) == Answer::kSendGo);
    CHECK(v.Tick(31000 + TakeoffVotePolicy::kBoardMs - 1) == Step::kNone);
    CHECK(v.LaunchRequested(32000, 1) == Request::kHold);      // boarding: no new vote
    CHECK(v.Tick(31000 + TakeoffVotePolicy::kBoardMs) == Step::kLaunch);
    CHECK(!v.Open());
}

TEST(TakeoffVote, EveryCrewMemberMustBeReady)
{
    TakeoffVotePolicy v;
    v.LaunchRequested(1000, 3);
    CHECK(v.Ready(v.Id(), 1, 2000) == Answer::kNone);
    CHECK(v.Ready(v.Id(), 1, 2500) == Answer::kNone);          // same player twice
    CHECK(v.Ready(v.Id(), 2, 3000) == Answer::kNone);
    CHECK_EQ(v.ReadyCount(), 2);
    CHECK(v.Ready(v.Id(), 3, 4000) == Answer::kSendGo);
}

TEST(TakeoffVote, StaleOrLateAnswersAreIgnored)
{
    TakeoffVotePolicy v;
    v.LaunchRequested(1000, 1);
    const auto first = v.Id();
    CHECK(v.Tick(1000 + TakeoffVotePolicy::kTimeoutMs) == Step::kTimedOut);
    CHECK(v.Ready(first, 1, 70000) == Answer::kNone);           // after the timeout
    CHECK(v.LaunchRequested(80000, 1) == Request::kHoldAndAsk);
    CHECK(v.Id() != first);
    CHECK(v.Ready(first, 1, 81000) == Answer::kNone);           // answer to the old vote
    CHECK(v.Ready(v.Id(), 1, 81000) == Answer::kSendGo);
}

TEST(TakeoffVote, CancelEndsTheVote)
{
    TakeoffVotePolicy v;
    v.LaunchRequested(1000, 1);
    v.Cancel();
    CHECK(!v.Open());
    CHECK(v.Ready(v.Id(), 1, 2000) == Answer::kNone);
    CHECK(v.Tick(1000 + TakeoffVotePolicy::kTimeoutMs) == Step::kNone);
}

TEST(TakeoffVote, ThreePlayersWaitForBoth)
{
    TakeoffVotePolicy v;
    CHECK(v.LaunchRequested(1000, 2) == Request::kHoldAndAsk);
    CHECK(v.Ready(v.Id(), 1, 2000) == Answer::kNone);       // player 1 ready
    CHECK(v.Ready(v.Id(), 1, 2500) == Answer::kNone);       // the same player again: still 1 of 2
    CHECK(v.ReadyCount() == 1);
    CHECK(v.Ready(v.Id(), 2, 9000) == Answer::kSendGo);     // player 2 ready: go
    CHECK(v.Tick(9000 + TakeoffVotePolicy::kBoardMs) == Step::kLaunch);
}
