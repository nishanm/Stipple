# name: Split Flap
# summary: A departure-board clock. Characters flip forward through the alphabet until they land on the time.
# author: Stipple
# tags: clock, animation, retro
# panel: 52x16

# @config flapms number "Milliseconds per flap" default=45 min=20 max=200 help="How fast the characters spin. Lower is faster."
# @config seconds boolean "Show seconds instead of the date" default=false

import string

class App
  var ALPHA
  var slots
  var shown, target
  var lastStep, flapms
  var withSeconds

  def init()
    # Forward only, like the real thing - a flap can never turn backwards, so
    # getting from Y to A means going all the way round. That is most of what
    # makes a departure board look like a departure board.
    self.ALPHA = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:."
    self.slots = 8
    self.flapms = store.get("flapms", 45)
    self.withSeconds = store.get("seconds", false)

    self.shown = []
    self.target = []
    var i = 0
    while i < self.slots * 2
      self.shown.push(0)
      self.target.push(0)
      i += 1
    end
    self.lastStep = 0
  end

  def duration()
    return 15000
  end

  def _index(ch)
    var i = 0
    while i < size(self.ALPHA)
      if self.ALPHA[i] == ch
        return i
      end
      i += 1
    end
    return 0
  end

  def _pad(s)
    # Centred in eight slots, so the board stays still while the characters
    # move rather than shuffling sideways as the text changes length.
    var t = s
    while size(t) < self.slots
      if size(t) % 2 == 0
        t = t + " "
      else
        t = " " + t
      end
    end
    return t[0 .. self.slots - 1]
  end

  def _want(row, s)
    var t = self._pad(s)
    var i = 0
    while i < self.slots
      self.target[row * self.slots + i] = self._index(t[i])
      i += 1
    end
  end

  def _flap(x, y, idx, spinning)
    var bg = rgb(16, 18, 24)
    if spinning
      bg = rgb(26, 29, 38)
    end
    rect_fill(x, y, 5, 7, bg)

    # The seam goes behind the character, not over it. A real flap cuts the
    # letter in half and that is the whole look - but the letter here is five
    # pixels by seven, and a black line through the middle of that costs more
    # legibility than the effect is worth. Behind it, the tile still reads as
    # two halves and the digits stay digits.
    line(x, y + 3, x + 4, y + 3, rgb(8, 9, 12))

    var ink = rgb(235, 240, 246)
    if spinning
      ink = rgb(150, 160, 175)
    end
    text(x, y, self.ALPHA[idx], ink)
  end

  def draw()
    clear(rgb(0, 0, 0))

    if !time_known()
      text(4, 5, "no clock", rgb(110, 110, 120))
      return
    end

    var top = string.format("%02d:%02d", hour(), minute())
    var bottom = ""
    if self.withSeconds
      bottom = string.format("%02d", second())
    else
      var names = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"]
      bottom = names[weekday()] + " " + str(day())
    end

    self._want(0, top)
    self._want(1, bottom)

    # One step per flap interval, and every slot steps together - a board
    # where each character ran at its own speed would look like noise rather
    # than like machinery.
    var now = now_ms()
    if now - self.lastStep >= self.flapms
      self.lastStep = now
      var i = 0
      while i < self.slots * 2
        if self.shown[i] != self.target[i]
          self.shown[i] = (self.shown[i] + 1) % size(self.ALPHA)
        end
        i += 1
      end
    end

    var left = int((width() - self.slots * 6 + 1) / 2)
    var s = 0
    while s < self.slots
      self._flap(left + s * 6, 0, self.shown[s],
                 self.shown[s] != self.target[s])
      self._flap(left + s * 6, 9, self.shown[self.slots + s],
                 self.shown[self.slots + s] != self.target[self.slots + s])
      s += 1
    end
  end
end

return App()
