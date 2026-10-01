# name: Weather
# summary: Live conditions from Open-Meteo - temperature, an animated sky, and the day's range.
# author: Stipple
# tags: http, weather, time, animation
# panel: 52x16

import string
import json

class App
  var URL
  var code, temp, lo, hi     # last parsed reading
  var have                   # whether those numbers mean anything
  var t                      # animation clock
  var drops                  # rain/snow particle offsets

  def init()

    var LAT = "52.37"
    var LON = "4.89"
    self.URL = "https://api.open-meteo.com/v1/forecast" +
               "?latitude=" + LAT + "&longitude=" + LON +
               "&current=temperature_2m,weather_code" +
               "&daily=temperature_2m_max,temperature_2m_min" +
               "&timezone=auto&forecast_days=1"
    self.code = -1
    self.temp = 0
    self.lo = 0
    self.hi = 0
    self.have = false
    self.t = 0
    self.drops = [0, 9, 18, 27, 36, 45]
  end

  # The carousel's default five seconds is not long enough to read a
  # temperature, a range and watch the sky move.
  def duration()
    return 9000
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

    var cur = doc.find("current")
    if cur == nil
      return
    end
    self.temp = int(cur.find("temperature_2m", 0))
    self.code = int(cur.find("weather_code", -1))

    var day = doc.find("daily")
    if day != nil
      var maxes = day.find("temperature_2m_max")
      var mins = day.find("temperature_2m_min")
      if maxes != nil && size(maxes) > 0
        self.hi = int(maxes[0])
      end
      if mins != nil && size(mins) > 0
        self.lo = int(mins[0])
      end
    end
    self.have = true
  end

  def draw()
    clear(rgb(0, 0, 0))
    self.t = now_ms()

    http_follow(self.URL, 600)

    if !http_known()
      text(2, 1, "no net", rgb(150, 60, 60))
      text(2, 9, "check wifi", rgb(70, 70, 70))
      return
    end

    self._parse()

    if !self.have
      # Three different nothings, and which one it is decides what you do
      # about it.
      var why = http_error(self.URL)
      if why != nil
        text(1, 1, "err", rgb(200, 70, 50))
        text(1, 9, why, rgb(90, 60, 60))
      else
        var code = http_status(self.URL)
        if code > 0 && (code < 200 || code >= 300)
          text(1, 1, "HTTP", rgb(200, 70, 50))
          text(1, 9, str(code), rgb(200, 70, 50))
        else
          text(1, 5, "fetching", rgb(70, 70, 70))
        end
      end
      return
    end

    self._sky()

    var label = str(self.temp) + "'"
    var w = text_width(label)
    text(51 - w, 0, label, self._warmth(self.temp))

    # The day's range underneath, dim. Useful in the morning and ignorable
    # the rest of the time, which is what a second line should be.
    var range = str(self.lo) + ".." + str(self.hi)
    var rw = text_width(range)
    text(51 - rw, 9, range, rgb(70, 78, 90))

    var age = http_age_ms(self.URL)
    if age > 0
      var bar = (age * 26) / 600000
      if bar > 26
        bar = 26
      end
      if bar > 0
        rect_fill(0, 15, bar, 1, age > 900000 ? rgb(90, 50, 20) : rgb(24, 32, 40))
      end
    end
  end

  # Blue when cold, through green, to red. Not a rainbow: three anchors is
  # enough to read at a glance and more would just be decoration.
  def _warmth(c)
    if c <= 0
      return rgb(120, 190, 255)
    elif c < 10
      return rgb(0, 200, 220)
    elif c < 18
      return rgb(120, 220, 120)
    elif c < 26
      return rgb(255, 190, 40)
    end
    return rgb(255, 90, 50)
  end

  # WMO weather codes, grouped. Open-Meteo documents about thirty of them and
  # a 52x16 panel can honestly distinguish five.
  def _sky()
    var c = self.code
    if c < 0
      return
    end

    if c == 0 || c == 1
      self._sun()
    elif c == 2 || c == 3 || c == 45 || c == 48
      self._cloud(c == 2)
    elif c >= 71 && c <= 77 || c == 85 || c == 86
      self._cloud(false)
      self._fall(rgb(220, 235, 255), 2)
    elif c >= 95
      self._cloud(false)
      self._storm()
    elif c >= 51
      self._cloud(false)
      self._fall(rgb(60, 140, 230), 4)
    else
      self._cloud(false)
    end
  end

  def _sun()
    var cx = 8
    var cy = 7
    rect_fill(cx - 2, cy - 2, 5, 5, rgb(255, 190, 30))
    pixel(cx - 3, cy - 1, rgb(255, 210, 60))
    pixel(cx + 3, cy - 1, rgb(255, 210, 60))
    pixel(cx - 1, cy - 3, rgb(255, 210, 60))
    pixel(cx - 1, cy + 3, rgb(255, 210, 60))

    # Eight rays, turning. The whole animation is one rotating index, which
    # is about as cheap as movement gets and still reads as alive.
    var step = (self.t / 250) % 8
    var k = 0
    while k < 4
      var a = (step + k * 2) % 8
      var dx = 0
      var dy = 0
      if a == 0 dx = 5 dy = 0
      elif a == 1 dx = 4 dy = 3
      elif a == 2 dx = 0 dy = 5
      elif a == 3 dx = -4 dy = 3
      elif a == 4 dx = -5 dy = 0
      elif a == 5 dx = -4 dy = -3
      elif a == 6 dx = 0 dy = -5
      else dx = 4 dy = -3
      end
      pixel(cx + dx - 1, cy + dy, rgb(180, 130, 0))
      k += 1
    end
  end

  def _cloud(sunny)
    if sunny
      rect_fill(4, 3, 3, 3, rgb(210, 160, 30))
    end
    # A cloud that drifts a pixel and back, so an overcast day is still a
    # panel that is doing something.
    var drift = ((self.t / 900) % 2)
    var x = 2 + drift
    rect_fill(x + 2, 6, 9, 3, rgb(150, 160, 175))
    rect_fill(x + 4, 4, 5, 2, rgb(180, 190, 205))
    rect_fill(x, 8, 13, 2, rgb(110, 120, 135))
  end

  def _fall(colour, speed)
    var i = 0
    while i < size(self.drops)
      var x = 2 + i * 2
      var y = 10 + ((self.t / (60 - speed * 8) + self.drops[i]) % 6)
      if y <= 15
        pixel(x, y, colour)
      end
      i += 1
    end
  end

  def _storm()
    # Two frames in every three seconds. A flash that is on half the time is
    # not a flash, it is a lamp.
    var phase = (self.t / 180) % 16
    if phase > 1
      return
    end
    pixel(7, 10, rgb(255, 240, 120))
    pixel(6, 11, rgb(255, 240, 120))
    pixel(8, 11, rgb(255, 240, 120))
    pixel(7, 12, rgb(255, 255, 200))
    pixel(6, 13, rgb(255, 240, 120))
  end
end

return App()
