# name: Plane Spotter
# summary: The nearest aircraft overhead - callsign, altitude, and an arrow pointing at which window to look out of.
# author: Stipple
# tags: http, aviation, map, tool
# panel: 52x16

# @config lat text "Latitude" default="52.3702" maxlen=12 help="Your position, in degrees. Save the script again after changing this."
# @config lon text "Longitude" default="4.8952" maxlen=12
# @config radius number "Search radius (nm)" default=10 min=2 max=25 help="Bigger finds more planes and downloads more; over a busy airport keep it small."
# @config refresh number "Check every (seconds)" default=60 min=30 max=600

import json
import math
import string

# Data from adsb.lol, which is free, needs no key, and is fed by volunteers
# with receivers on their roofs.

class App
  var URL, refresh, radius
  var following
  var flight, alt, dist, bearing, ground

  def init()
    var lat = store.get("lat", "52.3702")
    var lon = store.get("lon", "4.8952")
    self.radius = store.get("radius", 10)
    self.refresh = store.get("refresh", 60)
    self.URL = "https://api.adsb.lol/v2/point/" + lat + "/" + lon + "/" +
               str(self.radius)
    self.following = true
    self.flight = nil
  end

  def duration()
    return 15000
  end

  # The closest one that is actually flying. Anything on the ground is a plane
  # you cannot see from a window, and near an airport it would be most of them.
  def _pick()
    self.flight = nil
    var body = http_get(self.URL)
    if body == nil
      return
    end
    var doc = json.load(body)
    if doc == nil
      return
    end
    var list = doc.find("ac")
    if list == nil
      return
    end

    var best = nil
    var bestD = 9999
    var i = 0
    while i < size(list)
      var a = list[i]
      var d = a.find("dst")
      var alt = a.find("alt_baro")
      if d != nil && alt != "ground" && d < bestD
        bestD = d
        best = a
      end
      i += 1
    end

    if best == nil
      return
    end

    var call = best.find("flight")
    if call == nil
      call = best.find("hex")
    end
    if call == nil
      call = "?"
    end
    # The feed pads callsigns to eight characters with spaces.
    self.flight = string.split(str(call), " ")[0]
    if self.flight == ""
      self.flight = "?"
    end

    self.alt = best.find("alt_baro")
    self.dist = bestD
    self.bearing = best.find("dir")
    if self.bearing == nil
      self.bearing = -1
    end
  end

  # Flight level, which is how altitude is actually spoken: FL320 is 32,000
  # feet. Below the transition it is plain feet, so it stays plain here.
  def _alt()
    if self.alt == nil
      return "--"
    end
    var f = int(real(self.alt))
    if f >= 18000
      return "FL" + str(int(f / 100))
    end
    return str(int(f / 100) * 100) + "ft"
  end

  # An arrow from the middle of a 7x7 box, pointing the way you would have to
  # look. North is up, which is the only orientation anybody can use without
  # being told.
  def _arrow(cx, cy, deg, c)
    pixel(cx, cy - 3, rgb(40, 44, 52))
    pixel(cx, cy + 3, rgb(40, 44, 52))
    pixel(cx - 3, cy, rgb(40, 44, 52))
    pixel(cx + 3, cy, rgb(40, 44, 52))

    if deg < 0
      pixel(cx, cy, rgb(90, 90, 100))
      return
    end

    var rad = deg * math.pi / 180
    var dx = math.sin(rad)
    var dy = 0 - math.cos(rad)
    var tx = cx + int(dx * 3 + 0.5)
    var ty = cy + int(dy * 3 + 0.5)
    line(cx, cy, tx, ty, c)
    pixel(tx, ty, rgb(255, 255, 255))
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

    if http_age_ms(self.URL) < 0
      var why = http_error(self.URL)
      if why != nil
        text(0, 0, "err", rgb(200, 70, 50))
        text(0, 9, why, rgb(90, 60, 60))
      else
        text(4, 5, "scanning", rgb(80, 80, 90))
      end
      return
    end

    self._pick()

    if self.flight == nil
      text(1, 0, "no plane", rgb(70, 76, 90))
      text(1, 9, "overhead", rgb(50, 54, 66))
      return
    end

    # Closer is warmer, which is the only part of this somebody reads from
    # across the room.
    var c = rgb(90, 170, 255)
    if self.dist < 3
      c = rgb(255, 200, 60)
    elif self.dist < 6
      c = rgb(120, 220, 200)
    end

    text(0, 0, self.flight, rgb(230, 238, 245))
    self._arrow(3, 11, self.bearing, c)
    text(9, 8, self._alt(), c)

    # How close, as a bar: full width when it is overhead, gone at the edge of
    # the search. A number for this would be four characters there is no room
    # for, and the bar is the thing you glance at anyway.
    var near = self.radius - self.dist
    if near < 0
      near = 0
    end
    var w = int(near * width() / self.radius)
    if w > 0
      rect_fill(0, 15, w, 1, c)
    end
  end
end

return App()
