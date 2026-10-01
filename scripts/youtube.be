# name: YouTube
# summary: Live subscriber count for a channel you pick in the web page, no API key.
# author: Stipple
# tags: http, social, youtube, graph
# panel: 52x16

# @config chan text "Channel ID" default="UCpGLALzRO0uaasWTsm9M99w" maxlen=32 help="The UC... part of youtube.com/channel/UC..., not the @handle"
# @config views boolean "Show total views instead" default=false

import string
import json

class App
  var URL, chan
  var subs, prev
  var spark        # the last 52 readings, for the trend line
  var at

  def init()
    self.subs = -1
    self.prev = -1
    self.spark = []
    self.at = 0
    var i = 0
    while i < 52
      self.spark.push(-1)
      i += 1
    end
    self._point()
  end

  def _point()
    self.chan = store.get("chan", "UCpGLALzRO0uaasWTsm9M99w")
    self.URL = "https://api.socialcounts.org/youtube-live-subscriber-count/" + self.chan
  end

  def duration()
    return 8000
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
    var counters = doc.find("counters")
    if counters == nil
      return
    end
    # `api` is what YouTube last published, `estimation` is their guess
    # between publishes. The published one is the honest number.
    var block = counters.find("api")
    if block == nil
      block = counters.find("estimation")
    end
    if block == nil
      return
    end

    var key = store.get("views", false) ? "viewCount" : "subscriberCount"
    var value = block.find(key)
    if value == nil
      return
    end

    var n = int(value)
    if n < 0
      return
    end
    if n != self.subs
      self.prev = self.subs
      self.subs = n
      self.spark[self.at] = n
      self.at = (self.at + 1) % 52
    end
  end

  def _short(n)
    if n >= 1000000
      return string.format("%d.%dM", n / 1000000, (n % 1000000) / 100000)
    elif n >= 10000
      return string.format("%dK", n / 1000)
    elif n >= 1000
      return string.format("%d.%dK", n / 1000, (n % 1000) / 100)
    end
    return str(n)
  end

  def draw()
    clear(rgb(0, 0, 0))
    self._point()
    http_follow(self.URL, 120)

    if !http_known()
      text(2, 1, "no net", rgb(150, 60, 60))
      return
    end

    self._parse()

    if self.subs < 0
      var why = http_error(self.URL)
      if why != nil
        text(1, 1, "err", rgb(200, 70, 50))
        text(1, 9, why, rgb(90, 60, 60))
      elif http_status(self.URL) == 404
        # The commonest mistake by a distance, and worth naming: an @handle
        # is not a channel ID and the endpoint 404s on one.
        text(1, 1, "bad id", rgb(200, 70, 50))
        text(1, 9, "use UC..", rgb(90, 60, 60))
      else
        text(1, 5, "fetching", rgb(70, 70, 70))
      end
      return
    end

    self._logo()

    var label = self._short(self.subs)
    text(14, 0, label, rgb(240, 240, 240))

    var what = store.get("views", false) ? "views" : "subs"
    text(14, 9, what, rgb(70, 76, 90))

    self._trend()
  end

  def _logo()
    var lit = rgb(255, 94, 84)      # top edge, catching the light
    var body = rgb(216, 38, 33)     # the face
    var shade = rgb(158, 20, 18)    # bottom, in shadow
    var rim = rgb(96, 12, 11)       # the rounded corners

    var y = 0
    while y < 9
      var colour = body
      if y == 0
        colour = lit
      elif y == 1
        colour = rgb(238, 62, 54)
      elif y == 7
        colour = shade
      elif y == 8
        colour = rgb(126, 14, 13)
      end

      # The first and last rows stop one short at each end, and the pixel
      # they give up becomes the rim - that is the whole rounding.
      if y == 0 || y == 8
        line(1, 3 + y, 10, 3 + y, colour)
        pixel(0, 3 + y, rim)
        pixel(11, 3 + y, rim)
      else
        line(0, 3 + y, 11, 3 + y, colour)
      end
      y += 1
    end

    line(5, 5, 5, 9, rgb(255, 255, 255))
    line(6, 6, 6, 8, rgb(255, 255, 255))
    pixel(7, 7, rgb(255, 255, 255))
  end

  def _trend()
    var low = -1
    var high = -1
    var i = 0
    while i < 52
      var v = self.spark[i]
      if v >= 0
        if low < 0 || v < low low = v end
        if high < 0 || v > high high = v end
      end
      i += 1
    end
    if low < 0 || high <= low
      return
    end

    # Right half only: the number needs the left. Oldest at the left of the
    # strip, which is the direction every chart is read in.
    var x = 0
    while x < 16
      var idx = (self.at + 36 + x) % 52
      var v = self.spark[idx]
      if v >= 0
        var h = ((v - low) * 5) / (high - low)
        rect_fill(36 + x, 13 - h, 1, h + 1, rgb(0, 120, 90))
      end
      x += 1
    end
  end
end

return App()
