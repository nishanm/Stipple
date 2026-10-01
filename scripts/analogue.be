# name: Analogue
# summary: A real clock face with sweeping hands, the weekday and the date beside it.
# author: Stipple
# tags: clock, time, analogue
# panel: 52x16

import string

class App
  var SIN            # sine * 1000 for 60 positions, one per minute
  var CX, CY, R

  def init()
    self.CX = 25
    self.CY = 8
    self.R = 7

    self.SIN = [
         0,  105,  208,  309,  407,  500,  588,  669,  743,  809,
       866,  914,  951,  978,  995, 1000,  995,  978,  951,  914,
       866,  809,  743,  669,  588,  500,  407,  309,  208,  105,
         0, -105, -208, -309, -407, -500, -588, -669, -743, -809,
      -866, -914, -951, -978, -995,-1000, -995, -978, -951, -914,
      -866, -809, -743, -669, -588, -500, -407, -309, -208, -105]
  end

  # Cosine is sine a quarter turn along. One table, two functions.
  def _cos(i)
    return self.SIN[(i + 15) % 60]
  end

  def _hand(minute, length, colour)
    var x = self.CX + (self.SIN[minute] * length) / 1000
    var y = self.CY - (self._cos(minute) * length) / 1000
    line(self.CX, self.CY, x, y, colour)
  end

  def draw()
    clear(rgb(0, 0, 0))

    if !time_known()
      # A clock face with no time behind it is the most convincing lie this
      # panel could tell, because it looks exactly like a working clock.
      text(2, 5, "no time", rgb(180, 60, 60))
      return
    end

    var t = 0
    while t < 12
      var i = t * 5
      var x = self.CX + (self.SIN[i] * self.R) / 1000
      var y = self.CY - (self._cos(i) * self.R) / 1000
      pixel(x, y, (t % 3) == 0 ? rgb(90, 100, 120) : rgb(38, 44, 55))
      t += 1
    end

    var h = hour() % 12
    var m = minute()
    var s = second()

    # The hour hand advances with the minutes: at half past three it should
    # sit between three and four, not still on three.
    self._hand((h * 5 + m / 12) % 60, 4, rgb(0, 150, 220))
    self._hand(m, 6, rgb(235, 240, 255))

    var sx = self.CX + (self.SIN[s] * self.R) / 1000
    var sy = self.CY - (self._cos(s) * self.R) / 1000
    pixel(sx, sy, rgb(255, 130, 0))

    pixel(self.CX, self.CY, rgb(255, 255, 255))

    var days = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"]
    text(0, 1, days[weekday() % 7], rgb(70, 78, 92))
    text(34, 1, string.format("%02d", day()), rgb(150, 160, 175))
    text(34, 9, string.format("%02d", month()), rgb(70, 78, 92))

    # And the hour, bottom left, for the glance that wants a number. Two
    # digits only: the dial already says which side of the hour it is.
    text(0, 9, string.format("%02d", hour()), rgb(150, 160, 175))
  end
end

return App()
