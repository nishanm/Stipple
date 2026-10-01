# name: Pomodoro
# summary: Twenty-five minutes of work, five off, and a chime you do not have to watch for.
# author: Stipple
# tags: tool, audio, time, focus
# panel: 52x16

import string

class App
  var WORK, SHORT, LONG
  var phase          # 0 idle, 1 work, 2 short break, 3 long break
  var ends           # deadline on the device clock
  var paused, left   # when paused, what was left
  var done           # completed work blocks in this set
  var spoke

  def init()
    self.WORK = 25 * 60000
    self.SHORT = 5 * 60000
    self.LONG = 15 * 60000
    self.phase = 0
    self.ends = 0
    self.paused = false
    self.left = 0
    self.done = store.get("done", 0)
    self.spoke = -1
  end

  def _begin(phase, span, now)
    self.phase = phase
    self.ends = now + span
    self.paused = false
    self.spoke = -1
  end

  def on_button(name)
    var now = now_ms()

    if self.phase == 0
      self._begin(1, self.WORK, now)
    elif self.paused
      # Resume where it stopped rather than from the top.
      self.ends = now + self.left
      self.paused = false
    else
      self.paused = true
      self.left = self.ends - now
      if self.left < 0
        self.left = 0
      end
    end

    if audio_known()
      tone(1400, 20)
    end
  end

  # What follows this phase, and how long it lasts.
  def _next(now)
    if self.phase == 1
      self.done += 1
      store.set("done", self.done)
      if self.done % 4 == 0
        self._begin(3, self.LONG, now)
      else
        self._begin(2, self.SHORT, now)
      end
    else
      self._begin(1, self.WORK, now)
    end
  end

  def draw()
    clear(rgb(0, 0, 0))
    var now = now_ms()

    if self.phase == 0
      text(1, 1, "FOCUS", rgb(0, 150, 200))
      text(1, 9, "click", rgb(60, 66, 76))
      self._dots()
      return
    end

    var left = self.paused ? self.left : self.ends - now

    if left <= 0

      if audio_known() && self.spoke != 1
        self.spoke = 1
        tone(self.phase == 1 ? 1760 : 1320, 180)
        tone(self.phase == 1 ? 1320 : 1760, 180)
      end
      self._next(now)
      left = self.ends - now
    end

    var secs = left / 1000
    var span = self.phase == 1 ? self.WORK : (self.phase == 2 ? self.SHORT : self.LONG)

    # Work is warm, breaks are cool. The colour is the thing you read from
    # across the room; the digits are for when you are closer.
    var colour = rgb(255, 120, 40)
    if self.phase == 2
      colour = rgb(0, 190, 160)
    elif self.phase == 3
      colour = rgb(120, 140, 255)
    end
    if self.paused
      # Dimmed and blinking, because a paused timer that looks like a running
      # one is how a pomodoro session quietly becomes an afternoon.
      colour = (now / 500) % 2 == 0 ? rgb(90, 90, 90) : rgb(40, 40, 40)
    end

    text(1, 0, string.format("%02d:%02d", secs / 60, secs % 60), colour)

    # A bar that empties over the phase. 26 pixels so it sits under the
    # digits and leaves the right half for the dots.
    var used = span - left
    var w = 26 - (used * 26) / span
    if w > 0
      rect_fill(1, 8, w, 2, colour)
    end

    self._dots()
  end

  # Four blocks to a long break. Filled as they are earned, so the set is
  # visible without counting anything.
  def _dots()
    var earned = self.done % 4
    var i = 0
    while i < 4
      var x = 34 + i * 5
      if i < earned
        rect_fill(x, 11, 3, 3, rgb(255, 120, 40))
      else
        rect(x, 11, 3, 3, rgb(50, 40, 30))
      end
      i += 1
    end

    if !audio_known()
      # The chime is most of what this app is, so a device that cannot make
      # one should say so rather than silently being a clock.
      text(34, 1, "MUTE", rgb(120, 40, 40))
    end
  end
end

return App()
