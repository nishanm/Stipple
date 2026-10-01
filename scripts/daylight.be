# name: Daylight
# summary: Where the sun is in its arc today, from Open-Meteo, with the next event under it.
# author: Stipple
# tags: http, sun, time, animation
# panel: 52x16

import string
import json

class App
  var URL
  var rise, set      # minutes past midnight, -1 until fetched
  var have

  def init()
    var LAT = "52.37"
    var LON = "4.89"
    self.URL = "https://api.open-meteo.com/v1/forecast" +
               "?latitude=" + LAT + "&longitude=" + LON +
               "&daily=sunrise,sunset&timezone=auto&forecast_days=1"
    self.rise = -1
    self.set = -1
    self.have = false
  end

  def duration()
    return 8000
  end

  def _minutes(stamp)
    if stamp == nil || size(stamp) < 16
      return -1
    end
    var h = int(stamp[11..12])
    var m = int(stamp[14..15])
    if h < 0 || h > 23 || m < 0 || m > 59
      return -1
    end
    return h * 60 + m
  end

  def _parse()
    var body = http_get(self.URL)
    if body == nil
      return
    end
    var doc = json.load(body)
    if doc == nil
      return
    end
    var day = doc.find("daily")
    if day == nil
      return
    end
    var rises = day.find("sunrise")
    var sets = day.find("sunset")
    if rises == nil || sets == nil || size(rises) == 0 || size(sets) == 0
      return
    end
    var a = self._minutes(rises[0])
    var b = self._minutes(sets[0])
    if a < 0 || b < 0 || b <= a
      return
    end
    self.rise = a
    self.set = b
    self.have = true
  end

  def draw()
    clear(rgb(0, 0, 0))
    http_follow(self.URL, 1800)

    if !http_known()
      text(2, 1, "no net", rgb(150, 60, 60))
      return
    end

    self._parse()

    if !self.have
      var why = http_error(self.URL)
      if why != nil
        text(1, 1, "err", rgb(200, 70, 50))
        text(1, 9, why, rgb(90, 60, 60))
      else
        text(1, 5, "fetching", rgb(70, 70, 70))
      end
      return
    end

    if !time_known()
      # The arc needs to know where *now* is. Without a clock it would be a
      # picture of a sunrise on an unspecified day.
      text(2, 5, "no time", rgb(150, 60, 60))
      return
    end

    var now = hour() * 60 + minute()
    var up = now >= self.rise && now <= self.set

    # The arc. Baseline at y=8, peak at y=0, spanning x=2..49.
    var x = 2
    while x <= 49
      var t = ((x - 2) * 1000) / 47          # 0..1000 across the arc
      var off = t - 500
      # 8 - 8 * (1 - (2(t-0.5))^2), in integer arithmetic.
      var y = 8 - (8 * (250000 - off * off)) / 250000
      # Daylight ahead of the sun is dim, behind it is lit - so the arc fills
      # as the day passes and you can see the shape of what is left.
      var passed = up && t <= ((now - self.rise) * 1000) / (self.set - self.rise)
      pixel(x, y, passed ? rgb(90, 70, 20) : rgb(28, 30, 38))
      x += 1
    end

    # The horizon, so the arc has something to rise from.
    rect_fill(0, 9, 52, 1, rgb(24, 26, 32))

    if up
      var t = ((now - self.rise) * 1000) / (self.set - self.rise)
      var sx = 2 + (t * 47) / 1000
      var off = t - 500
      var sy = 8 - (8 * (250000 - off * off)) / 250000
      rect_fill(sx - 1, sy - 1, 3, 3, rgb(255, 200, 40))
      pixel(sx, sy, rgb(255, 255, 220))
    end

    var event = up ? self.set : self.rise
    var label = string.format("%02d:%02d", event / 60, event % 60)
    var colour = up ? rgb(255, 140, 40) : rgb(120, 170, 255)

    # A triangle for which way the sun is about to go, drawn rather than
    # spelled: there is no arrow in a 5x7 font.
    var ax = 2
    var ay = 11
    var k = 0
    while k < 3
      if up
        line(ax + 2 - k, ay + k, ax + 2 + k, ay + k, colour)
      else
        line(ax + k, ay + 2 - k, ax + 4 - k, ay + 2 - k, colour)
      end
      k += 1
    end

    text(8, 9, label, colour)
  end
end

return App()
