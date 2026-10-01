# name: Bin Day
# summary: Which bin goes out next, and how long you have. Unglamorous, and the one you keep on screen.
# author: Stipple
# tags: clock, home, tool

# @config name1 text "First bin" default="GENERAL" maxlen=10
# @config day1 number "Collected on" default=2 min=0 max=6 help="0 Sunday, 1 Monday, 2 Tuesday, 3 Wednesday, 4 Thursday, 5 Friday, 6 Saturday."
# @config hue1 number "Colour" default=35 min=0 max=359
# @config name2 text "Second bin" default="RECYCLE" maxlen=10
# @config day2 number "Collected on" default=5 min=0 max=6
# @config hue2 number "Colour" default=200 min=0 max=359

class App
  var bins

  def init()
    self.bins = [
      [store.get("name1", "GENERAL"), store.get("day1", 2), self._hue(store.get("hue1", 35))],
      [store.get("name2", "RECYCLE"), store.get("day2", 5), self._hue(store.get("hue2", 200))]
    ]
  end

  def duration()
    return 12000
  end

  def _hue(h)
    var x = h % 360
    var seg = int(x / 60)
    var f = x % 60
    var up = int(f * 230 / 60)
    var dn = 230 - up
    if seg == 0
      return rgb(230, up, 0)
    elif seg == 1
      return rgb(dn, 230, 0)
    elif seg == 2
      return rgb(0, 230, up)
    elif seg == 3
      return rgb(0, dn, 230)
    elif seg == 4
      return rgb(up, 0, 230)
    end
    return rgb(230, 0, dn)
  end

  def _dim(c, on)
    if on
      return c
    end
    return rgb(70, 74, 84)
  end

  # Days until the next one, counting today as zero.
  def _until(day)
    return (day - weekday() + 7) % 7
  end

  def _when(days)
    if days == 0
      return "NOW"
    end
    return str(days) + "d"
  end

  # Cut to whatever is left after the countdown has taken its space. A name
  # that runs under the number is worse than a shortened one, and text past
  # pixel 51 is clipped silently rather than wrapped.
  def _fit(name, room)
    var s = name
    while size(s) > 0 && text_width(s) > room
      s = s[0 .. size(s) - 2]
    end
    return s
  end

  def _row(y, bin, soonest)
    var days = self._until(bin[1])
    var due = days == 0
    var colour = self._dim(bin[2], due || days == soonest)

    rect_fill(0, y + 1, 3, 5, colour)

    var when = self._when(days)
    var wide = text_width(when)
    var room = width() - 5 - wide - 2
    text(5, y, self._fit(bin[0], room), colour)

    # The countdown is the part somebody is actually reading, so it keeps its
    # colour even when the row is dimmed.
    var ink = rgb(120, 126, 140)
    if due
      ink = rgb(255, 255, 255)
    elif days == soonest
      ink = bin[2]
    end
    text(width() - wide, y, when, ink)
  end

  def draw()
    clear(rgb(0, 0, 0))

    if !time_known()
      # Without a date this cannot know what day it is, and a bin day it
      # guessed would be worse than none.
      text(4, 5, "no clock", rgb(110, 110, 120))
      return
    end

    var a = self._until(self.bins[0][1])
    var b = self._until(self.bins[1][1])
    var soonest = a < b ? a : b

    self._row(0, self.bins[0], soonest)
    self._row(9, self.bins[1], soonest)
  end
end

return App()
