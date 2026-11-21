// SPDX-License-Identifier: MPL-2.0
.pragma library

// The mock clock is 13 Nov 2026, 15:00 in HCMC. No timers, providers or scheduler run.
var now = Date.UTC(2026, 10, 13, 8, 0)
var zones = ["Asia/Ho_Chi_Minh", "UTC", "Asia/Tokyo"]
var offsets = [7, 0, 9]
var rules = ["Once", "Daily", "Weekdays", "Weekly", "Every N hours"]
var days = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
function pad(n) { return String(n).padStart(2, "0") }
function timestamp(date, time, zone) {
    if (!/^\d{4}-\d{2}-\d{2}$/.test(date) || !/^([01]\d|2[0-3]):[0-5]\d$/.test(time)) return NaN
    var d = date.split("-").map(Number), t = time.split(":").map(Number)
    var check = new Date(Date.UTC(d[0], d[1] - 1, d[2]))
    if (check.getUTCFullYear() !== d[0] || check.getUTCMonth() !== d[1] - 1 || check.getUTCDate() !== d[2]) return NaN
    return Date.UTC(d[0], d[1] - 1, d[2], t[0] - offsets[Math.max(0, zones.indexOf(zone))], t[1])
}
function startError(date, time, zone) {
    var at = timestamp(date, time, zone)
    if (!isFinite(at)) return "Enter a valid date (YYYY-MM-DD) and time (HH:mm)."
    if (at < now + 60000) return "Choose a start time at least 1 minute from now."
    if (at > now + 30 * 86400000) return "Choose a start time within the next 30 days."
    return ""
}
function error(draft) {
    if (!draft.name.trim()) return "Name is required"
    if (!draft.prompt.trim()) return "Prompt is required"
    if (draft.rule === 0) return startError(draft.date, draft.time, draft.zone)
    if (!/^([01]\d|2[0-3]):[0-5]\d$/.test(draft.time)) return "Enter a valid time (HH:mm)"
    if (draft.rule === 3 && draft.days.length === 0) return "Choose at least one weekday"
    if (draft.rule === 4 && (!/^\d+$/.test(String(draft.hours)) || Number(draft.hours) < 1 || Number(draft.hours) > 168))
        return "Enter a whole number of hours from 1 to 168"
    return ""
}
function next(d) {
    if (d.rule === 0) return timestamp(d.date, d.time, d.zone)
    if (d.rule === 4) return now + Number(d.hours) * 3600000
    var offset = offsets[Math.max(0, zones.indexOf(d.zone))]
    var local = new Date(now + offset * 3600000), t = d.time.split(":").map(Number)
    for (var i = 0; i < 8; i++) {
        var at = Date.UTC(local.getUTCFullYear(), local.getUTCMonth(), local.getUTCDate() + i, t[0] - offset, t[1])
        var day = new Date(at + offset * 3600000).getUTCDay()
        if (at <= now) continue
        if (d.rule === 2 && (day === 0 || day === 6)) continue
        if (d.rule === 3 && d.days.indexOf(day) < 0) continue
        return at
    }
    return NaN
}
function nextLabel(d) {
    var at = next(d)
    if (!isFinite(at)) return "choose a day"
    var offset = offsets[Math.max(0, zones.indexOf(d.zone))]
    var date = new Date(at + offset * 3600000)
    var delta = Math.floor((at + offset * 3600000) / 86400000) - Math.floor((now + offset * 3600000) / 86400000)
    return (delta === 0 ? "today" : delta === 1 ? "tomorrow" : pad(date.getUTCDate()) + "/" + pad(date.getUTCMonth() + 1))
        + " " + pad(date.getUTCHours()) + ":" + pad(date.getUTCMinutes())
}
function summary(d) {
    if (d.rule === 0) return "Once · " + d.date + " " + d.time
    if (d.rule === 3) return d.days.map(day => days[day]).join(", ") + " " + d.time
    if (d.rule === 4) return "Every " + d.hours + " hours"
    return rules[d.rule] + " " + d.time
}
function liveSummary(d) {
    var offset = offsets[Math.max(0, zones.indexOf(d.zone))]
    return summary(d) + " (GMT" + (offset > 0 ? "+" : "") + offset + ") · next: " + nextLabel(d)
}
