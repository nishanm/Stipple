# name: Air Quality
# summary: European AQI and particulates for your street, colour-coded, from Open-Meteo.
# author: Stipple
# tags: http, weather, health, home
# panel: 52x16

# @config lat text "Latitude" default="52.3702" maxlen=12 help="Your position, in degrees. Save the script again after changing this."
# @config lon text "Longitude" default="4.8952" maxlen=12
# @config refresh number "Check every (seconds)" default=1800 min=600 max=21600

import json
import string

# Open-Meteo needs no key and answers in under four hundred bytes.

class App
  var URL, refresh
  var following
  var BANDS, COLS

  def init()
    var lat = store.get("lat", "52.3702")
    var lon = store.get("lon", "4.8952")
    self.refresh = store.get("refresh", 1800)
    self.URL = "https://air-quality-api.open-meteo.com/v1/air-quality" +
               "?latitude=" + lat + "&longitude=" + lon +
               "&current=pm2_5,pm10,european_aqi"
    self.following = true

    # The European AQI bands, which are the ones the number is on. Naming the
    # band matters more than the number does: 43 means nothing to most people
    # and "FAIR" means something to everybody.
    self.BANDS = ["GOOD", "FAIR", "MOD", "POOR", "VERY", "HARM"]
    self.COLS = [
      rgb(80, 200, 120), rgb(170, 210, 70), rgb(240, 200, 60),
      rgb(240, 140, 50), rgb(230, 70, 70), rgb(160, 60, 180)
    ]
  end

  def duration()
    return 15000
  end

  def _band(aqi)
    if aqi <= 20
      return 0
    elif aqi <= 40
      return 1
    elif aqi <= 60
      return 2
    elif aqi <= 80
      return 3
    elif aqi <= 100
      return 4
    end
    return 5
  end

  def draw()
    clear(rgb(0, 0, 0))

    if !http_known()
      text(2, 0, "no", rgb(120, 120, 120))
      text(2, 9, "net", rgb(150, 70, 60))
      return
    end

    self.following = http_follow(self.URL, self.refresh)
    if !self.following
      text(0, 0, "re-save", rgb(230, 150, 60))
      text(0, 9, "to apply", rgb(120, 90, 50))
      return
    end

    var body = http_get(self.URL)
    if body == nil
      var why = http_error(self.URL)
      if why != nil
        text(0, 0, "err", rgb(200, 70, 50))
        text(0, 9, why, rgb(90, 60, 60))
      else
        text(4, 5, "sampling", rgb(80, 80, 90))
      end
      return
    end

    var doc = json.load(body)
    if doc == nil
      text(2, 5, "bad reply", rgb(160, 90, 60))
      return
    end
    var cur = doc.find("current")
    if cur == nil
      text(2, 5, "no reading", rgb(160, 90, 60))
      return
    end

    var aqi = cur.find("european_aqi")
    if aqi == nil
      text(2, 5, "no aqi", rgb(160, 90, 60))
      return
    end
    aqi = int(real(aqi))

    var band = self._band(aqi)
    var c = self.COLS[band]

    text(0, 0, str(aqi), c)
    var word = self.BANDS[band]
    text(width() - text_width(word), 0, word, c)

    var pm = cur.find("pm2_5")
    text(0, 8, "PM2.5", rgb(80, 86, 98))
    if pm == nil
      text(width() - text_width("--"), 8, "--", rgb(80, 86, 98))
    else
      var v = str(int(real(pm)))
      text(width() - text_width(v), 8, v, rgb(190, 200, 210))
    end

    # The scale, with a marker where this reading sits. A number on its own
    # does not say whether 43 is nearly clean or nearly bad.
    var x = 0
    while x < width()
      var at = int(x * 6 / width())
      pixel(x, 15, self.COLS[at])
      x += 1
    end
    var mark = int(aqi * width() / 120)
    if mark > width() - 1
      mark = width() - 1
    end
    pixel(mark, 15, rgb(255, 255, 255))
    if mark > 0
      pixel(mark - 1, 14, rgb(90, 90, 100))
    end
    pixel(mark, 14, rgb(255, 255, 255))
  end
end

return App()
