# name: Kitchen Timer
# summary: Click up the minutes, walk away, and it shouts when the time is up.
# author: Stipple
# tags: audio, tool, time
# panel: 52x16

import string

class App
  var mode         # 0 setting, 1 running, 2 finished
  var mins         # minutes dialled in
  var touched      # when the last click landed
  var ends         # deadline, on the device clock
  var spoke        # the last second we made a noise on

  def init()
    self.mode = 0
    self.mins = 0
    self.touched = 0
    self.ends = 0
    self.spoke = -1
  end

  def on_button(name)
    var now = now_ms()
    if self.mode == 0
      self.mins += 1
      if self.mins > 60
        self.mins = 1
      end
      self.touched = now
      if audio_known()
        tone(1200, 15)
      end
    else
      # Cancel, or silence the alarm. Same click, because by the time you
      # are reaching for the device you want it to stop either way.
      self.mode = 0
      self.mins = 0
      self.spoke = -1
    end
  end

  # Seconds left, floored, never negative.
  def _left(now)
    var ms = self.ends - now
    if ms < 0
      return 0
    end
    return ms / 1000
  end

  def _clock(x, y, secs, colour)
    text(x, y, string.format("%02d:%02d", secs / 60, secs % 60), colour)
  end

  def draw()
    clear(rgb(0, 0, 0))
    var now = now_ms()

    if self.mode == 0
      self._setting(now)
    elif self.mode == 1
      self._running(now)
    else
      self._finished(now)
    end
  end

  def _setting(now)
    if self.mins == 0

      var pulse = (now / 8) % 250
      if pulse > 125
        pulse = 250 - pulse
      end
      self._clock(1, 1, 0, rgb(0, 40 + pulse / 2, 60 + pulse))
      text(38, 1, "+1", rgb(150, 110, 0))

      if !audio_known()
        text(1, 9, "MUTE", rgb(130, 40, 40))
      end
      return
    end

    self._clock(1, 1, self.mins * 60, rgb(0, 190, 255))

    # A two-second grace period, drawn as a bar that empties. Without it,
    # dialling in ten minutes would mean ten starts and nine cancels.
    var wait = 2000 - (now - self.touched)
    if wait <= 0
      self.mode = 1
      self.ends = now + self.mins * 60000
      self.spoke = -1
      return
    end
    var w = (wait * 50) / 2000
    rect_fill(1, 11, w, 3, rgb(0, 90, 120))
  end

  def _running(now)
    var left = self._left(now)
    if left <= 0
      self.mode = 2
      self.ends = now
      self.spoke = -1
      return
    end

    # White for most of it, amber under a minute, red under ten seconds -
    # so it reads from across the room without counting digits.
    var colour = rgb(230, 230, 230)
    if left < 10
      colour = rgb(255, 70, 40)
    elif left < 60
      colour = rgb(255, 176, 0)
    end
    self._clock(1, 1, left, colour)

    if left < 10 && left != self.spoke
      self.spoke = left
      if audio_known()
        tone(1800, 18)
      end
    end

    var total = self.mins * 60
    var w = (left * 50) / total
    rect_fill(1, 11, w, 3, colour)
    rect(0, 10, 52, 5, rgb(30, 30, 30))
  end

  def _finished(now)
    var since = now - self.ends

    if since > 60000
      self.mode = 0
      self.mins = 0
      return
    end

    var on = (since / 500) % 2 == 0
    if on
      clear(rgb(90, 0, 0))
      text(8, 5, "TIME", rgb(255, 255, 255))
    else
      text(8, 5, "TIME", rgb(120, 20, 20))
    end

    var half = since / 500
    if half != self.spoke
      self.spoke = half
      if audio_known()
        # Two notes rather than one. A single pitch repeating is easy for a
        # room to filter out; an interval is not.
        tone(2200, 130)
        tone(1650, 130)
      end
    end
  end
end

return App()
