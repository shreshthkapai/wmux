# Scrollback And Mouse Routing

wmux keeps scrollback ownership in the server and native input decoding in the
platform client.

## Ownership

Pane history is canonical server state. Completed logical lines live in the
pane grid and are reflowed lazily by requested width. A client stores only its
own offset from the live bottom for each pane. It never owns or mutates pane
history.

This gives two clients attached to one session independent views. New PTY
output continues to update the pane while either client is scrolled. The
server advances a scrolled client's offset by the pane's monotonic
history-added counter, preserving the viewed content until history eviction
makes that impossible.

Historical rendering uses a `PaneViewport` snapshot derived from the
authoritative grid. It enters the normal scene diff renderer, hides the live
application cursor in a scrolled pane, and does not replay PTY bytes or move
the pane cursor. Returning to offset zero resumes the ordinary damage-journal
path.

## Structured Input

Platform backends produce `wmux_platform::MouseEvent` values with zero-based
cell coordinates, event kind, button, and modifiers. Protocol version 9 carries
that event as a fixed-size frame. No platform console records or pre-encoded
mouse escape strings cross the boundary.

The Windows client requests native mouse and resize input while explicitly
clearing Quick Edit, line input, echo, and processed input. Attached clients
enable xterm button-event plus SGR reporting on the outer terminal for the
duration of an attachment and restore both output and console input modes on
exit. Button-event tracking retains wheel, click, release, and drag input
without flooding IPC with unpressed pointer motion.

## Routing

The state owner hit-tests the event against the current structural scene:

1. A plain left press on an inactive pane selects it and schedules a structural
   redraw so every attached view sees the new active pane. The press, any drag,
   and its release are consumed as one focus gesture and do not reach the newly
   active application.
2. A left press in a pane without application mouse tracking remains pending
   until movement. A release without movement is a plain click; the first drag
   enters server-owned copy mode at the original press cell. Release copies the
   selection and returns to the live pane.
3. Alt or Shift plus left drag takes the same selection path even when the
   application requested mouse tracking. Alt or Shift plus right-click, or an
   ordinary right-click without application tracking, queues the newest wmux
   buffer for bracketed paste.
4. Outside those multiplexer gestures, if the target pane has requested DEC
   mouse tracking, wmux translates the event to pane-relative coordinates and
   sends it to that pane.
5. Otherwise, if the application has enabled alternate scroll (`1007`) and its
   alternate screen is active, wmux translates each vertical wheel event to
   three cursor-navigation steps and sends the batch to that pane in one PTY
   write. This keeps full-screen and inline TUIs responsive without multiplying
   IPC or backend writes.
6. Otherwise, wheel-up moves that client's target-pane viewport five rows into
   history.
7. Wheel-down moves five rows toward the live bottom and leaves history view at
   offset zero.
8. In explicit copy mode, wheel and left press, drag, and release events are
   consumed by the client's server-owned copy state before application routing.
9. Other button events do not move the pane application's authoritative cursor.

Supported application modes are X10 (`9`), normal (`1000`), button-event
(`1002`), any-event (`1003`), UTF-8 coordinates (`1005`), SGR (`1006`), and
urxvt (`1015`). Motion and release filtering follows the requested tracking
level. SGR is preferred when requested, followed by urxvt, UTF-8, and legacy
xterm encoding.

Mouse wheel input is translated into cursor navigation only when the
application explicitly requests alternate scroll and its alternate screen is
active. Applications that request mouse input receive it; remaining wheel input
is multiplexer history navigation.

Every wheel, press, drag, and release event is processed in arrival order even
when physical rendering is behind. History offsets clamp at their authoritative
limit, repeated wheel-down reaches the live view exactly, and application mouse
tracking emits one ordered report for every wheel event. Visual updates may
coalesce behind a client's one-frame presentation gate, but mouse events,
selection state, clipboard payloads, and pane input are never dropped or
merged. Platform regression tests preserve complete down-drag-up sequences,
coordinates, buttons, and modifiers on both native input backends.

Normal keyboard input and paste that are routed to a pane clear only the
originating client's historical offset for that pane and request an immediate
coherent render of the live view. Other attached clients retain their own
offsets. Input handled by an explicit copy mode remains copy-mode input and
does not reach the pane or force an exit from that mode.

Copy-mode navigation, selection, search, and clipboard transfer are documented
in `docs/copy-mode.md`.

Existing wmux lazy history, structural scene, damage journal, and independent
client baseline contracts remain authoritative.

## Inline Viewports And Scrolling Regions

Terminal applications can keep an editable viewport at the bottom of the
screen while finalizing completed output above it. When a line feed or an
explicit scroll-up operation moves rows out of a scrolling region anchored at
row zero, wmux records those rows in the pane's canonical history even if the
region ends above the physical screen bottom. Regions that do not include row
zero remain isolated from history.

Explicit scroll-up, scroll-down, and reverse-index operations honor the active
scroll margins. Reverse index scrolls the region downward when the cursor is at
its top margin and otherwise moves the cursor up without changing pane
history. These rules let inline applications redraw their live viewport while
completed transcript rows remain available to wheel and copy-mode navigation.
