// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtTest
import "../qml/parts"

TestCase {
    name: "PlanRunModel"
    PlanRunModel { id: run; conversationId: "4821" }
    Component { id: storeFactory; ConversationPlanStore {} }
    function init() { run.clock.interval = 2200; run.load("ready") }
    function test_prompt_reads_repo_context_before_creating_plan() {
        run.submitPrompt()
        compare(run.status, "planning")
        verify(!run.contextRead)
        verify(!run.planCreated)
        run.advance()
        verify(run.contextRead)
        verify(!run.planCreated)
        compare(run.completed, 0)
        run.advance()
        verify(run.planCreated)
        compare(run.status, "running")
        compare(run.completed, 0)
        verify(run.tasks.every(t => t.source.length > 0))
    }
    function test_stop_during_context_read_does_not_skip_planning() {
        run.submitPrompt(); run.stop()
        run.advance()
        verify(!run.contextRead)
        verify(!run.planCreated)
        run.resume()
        compare(run.status, "planning")
        run.advance(); run.advance()
        verify(run.planCreated)
        compare(run.status, "running")
        compare(run.completed, 0)
    }
    function test_early_finish_keeps_open_tasks_and_resumes() {
        run.start(); run.advance()
        compare(run.status, "continuing")
        compare(run.completed, 1)
        compare(run.followUps, 0)
        run.advance()
        compare(run.status, "running")
        compare(run.followUps, 1)
        compare(run.completed, 1)
        run.advance(); run.advance(); run.advance()
        compare(run.status, "complete")
        compare(run.completed, 4)
        compare(run.followUps, 1)
        for (let i = 0; i < 4; ++i)
            verify(run.events.some(e => e.detail === run.tasks[i].evidence))
    }
    function test_stop_cancels_queued_followup() {
        run.start(); run.advance(); run.stop()
        compare(run.status, "paused")
        verify(!run.autoplay)
        const count = run.events.length
        run.advance(); run.advance()
        compare(run.followUps, 0)
        compare(run.completed, 1)
        compare(run.events.length, count)
        run.resume(); run.advance()
        compare(run.completed, 2)
    }
    function test_blocker_cannot_be_marked_done_by_retry() {
        run.load("blocked")
        run.advance(); run.resume(); run.retryBlocker()
        compare(run.status, "blocked")
        compare(run.completed, 1)
        compare(run.followUps, 0)
        verify(!run.autoplay)
    }
    function test_no_progress_exhausts_followup_budget() {
        run.start(); run.advance()
        for (let i = 0; i < 3; ++i) {
            run.advance(); run.earlyFinish()
        }
        run.advance()
        compare(run.status, "limit")
        compare(run.completed, 1)
        compare(run.followUps, 3)
        verify(!run.autoplay)
        run.advance(); run.resume()
        compare(run.status, "limit")
        run.extendLimit(); run.advance()
        compare(run.followUps, 4)
        compare(run.followUpLimit, 4)
        compare(run.completed, 1)
    }
    function test_completed_plan_is_terminal() {
        run.load("complete")
        const count = run.events.length
        run.start(); run.advance(); run.earlyFinish(); run.resume(); run.stop(); run.extendLimit()
        compare(run.status, "complete")
        compare(run.completed, 4)
        compare(run.events.length, count)
        verify(!run.autoplay)
    }
    function test_plan_belongs_to_conversation_and_survives_switch() {
        const store = createTemporaryObject(storeFactory, this)
        const first = store.get("4821")
        first.load("continuing")
        first.stop()
        const second = store.get("4822")
        compare(second.status, "ready")
        compare(second.completed, 0)
        second.load("complete")
        compare(store.get("4821"), first)
        compare(first.status, "paused")
        compare(first.completed, 1)
        compare(first.followUps, 0)
        verify(first.events.some(e => e.detail.indexOf("@agent/4821") >= 0))
        verify(!first.events.some(e => e.detail.indexOf("@agent/4822") >= 0))
        compare(second.completed, 4)
    }
    function test_timer_finishes_without_another_user_prompt() {
        run.clock.interval = 5
        run.submitPrompt()
        tryCompare(run, "status", "complete", 2000)
        compare(run.completed, 4)
        compare(run.followUps, 1)
        verify(!run.autoplay)
    }
    function test_stop_prevents_timer_followup() {
        run.clock.interval = 5
        run.start(); run.advance(); run.stop()
        wait(40)
        compare(run.status, "paused")
        compare(run.completed, 1)
        compare(run.followUps, 0)
    }
    function test_direct_scenes_do_not_autoplay() {
        for (const state of ["ready", "planning", "running", "continuing", "blocked", "paused", "limit", "complete"]) {
            run.load(state)
            compare(run.status, state)
            verify(!run.autoplay)
        }
    }
}
