# name: Internet Monitor
# summary: Your public IPv4 and whether the internet is actually up, checked against two independent services.
# author: Stipple
# tags: http, network, status, tool
# panel: 52x16

# @config refresh number "Check every (seconds)" default=300 min=60 max=3600 help="How often to ask. Free services are free because nobody hammers them."
# @config fails number "Offline after this many missed checks" default=3 min=1 max=10 help="One failed check is usually a hiccup. This is how many in a row it takes before the panel says OFFLINE."
# @config rainbow boolean "Colour the address" default=true
# @config url1 text "Address service" default="https://api.ipify.org" maxlen=120 help="Must return your public IPv4 somewhere in the reply. Save the script again after changing this."
# @config url2 text "Backup service" default="https://checkip.amazonaws.com" maxlen=120 help="Asked only once the first one has failed, so one service being down is not reported as the internet being down."

import string

# Ported from Spectral's AWTRIX NG script:
# https://git.mike-lindner.net/mike/awtrix-ng-internet-monitor

class App
  var U1, U2
  var DIGITS
  var refresh, graceMs, rainbow
  var ip, following

  def init()

    self.U1 = store.get("url1", "https://api.ipify.org")
    self.U2 = store.get("url2", "https://checkip.amazonaws.com")

    self.DIGITS = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]

    self.refresh = store.get("refresh", 300)
    var n = store.get("fails", 3)
    self.graceMs = self.refresh * n * 1000
    self.rainbow = store.get("rainbow", true)

    # Remembered across a restart, so a device that boots while the line is
    # down can still tell you what the address was.
    self.ip = store.get("ip", nil)
    self.following = true
  end

  def duration()
    return 12000
  end

  # --- parsing -------------------------------------------------------------

  def _dig(c)
    var i = 0
    while i < 10
      if c == self.DIGITS[i]
        return i
      end
      i += 1
    end
    return -1
  end

  def _find(body)
    if body == nil
      return nil
    end
    var n = size(body)
    if n > 256
      n = 256
    end

    var i = 0
    while i < n
      if self._dig(body[i]) >= 0
        var start = i
        var j = i
        var groups = 0
        var ok = true

        while groups < 4
          var digits = 0
          var value = 0
          while j < n && self._dig(body[j]) >= 0
            value = value * 10 + self._dig(body[j])
            digits += 1
            j += 1
          end
          if digits < 1 || digits > 3 || value > 255
            ok = false
            break
          end
          groups += 1
          if groups < 4
            if j < n && body[j] == "."
              j += 1
            else
              ok = false
              break
            end
          end
        end

        if ok && groups == 4
          return body[start .. j - 1]
        end
        i = start + 1
      else
        i += 1
      end
    end
    return nil
  end

  def _ages()
    var bestIp = -1
    var bestAny = -1
    var found = nil

    var i = 0
    var urls = [self.U1, self.U2]
    while i < 2
      var a = http_age_ms(urls[i])
      if a >= 0
        if bestAny < 0 || a < bestAny
          bestAny = a
        end
        var v = self._find(http_get(urls[i]))
        if v != nil && (bestIp < 0 || a < bestIp)
          bestIp = a
          found = v
        end
      end
      i += 1
    end

    if found != nil && found != self.ip
      self.ip = found
      store.set("ip", found)
    end
    return [bestIp, bestAny]
  end

  # --- drawing -------------------------------------------------------------

  def _hue(h)
    var x = h % 360
    var seg = int(x / 60)
    var f = x % 60
    var up = int(f * 255 / 60)
    var dn = 255 - up
    if seg == 0
      return rgb(255, up, 0)
    elif seg == 1
      return rgb(dn, 255, 0)
    elif seg == 2
      return rgb(0, 255, up)
    elif seg == 3
      return rgb(0, dn, 255)
    elif seg == 4
      return rgb(up, 0, 255)
    end
    return rgb(255, 0, dn)
  end

  def _tick(ox, oy, c)
    line(ox, oy + 3, ox + 1, oy + 4, c)
    line(ox + 1, oy + 4, ox + 4, oy + 1, c)
  end

  def _cross(ox, oy, c)
    line(ox, oy, ox + 4, oy + 4, c)
    line(ox, oy + 4, ox + 4, oy, c)
  end

  def _split(ip)
    var seen = 0
    var i = 0
    while i < size(ip)
      if ip[i] == "."
        seen += 1
        if seen == 2
          return [ip[0 .. i], ip[i + 1 .. size(ip) - 1]]
        end
      end
      i += 1
    end
    return [ip, ""]
  end

  def _address(y, plain)
    var parts = self._split(self.ip)
    var top = parts[0]
    var bottom = parts[1]

    if !self.rainbow || plain
      text(0, y, top, rgb(200, 210, 220))
      text(0, y + 8, bottom, rgb(200, 210, 220))
      return
    end

    var phase = int(now_ms() / 70) % 360
    var x = 0
    var i = 0
    while i < size(top)
      x += text(x, y, top[i], self._hue(phase + i * 14))
      i += 1
    end

    x = 0
    var k = 0
    while k < size(bottom)
      x += text(x, y + 8, bottom[k], self._hue(phase + (i + k) * 14))
      k += 1
    end
  end

  def _ago(ms)
    var s = int(ms / 1000)
    if s < 90
      return str(s) + "s"
    end
    var m = int(s / 60)
    if m < 90
      return str(m) + "m"
    end
    return str(int(m / 60)) + "h"
  end

  def draw()
    clear(rgb(0, 0, 0))

    if !http_known()
      # Not the same as being offline, and worth the distinction: this is the
      # device having no network at all rather than the internet being down.
      text(2, 0, "no", rgb(120, 120, 120))
      text(2, 9, "net", rgb(150, 70, 60))
      return
    end

    # Every frame, because there is nowhere else to ask. Asking again is free.
    self.following = http_follow(self.U1, self.refresh)

    if http_error(self.U1) != nil || http_age_ms(self.U1) > self.graceMs
      http_follow(self.U2, self.refresh)
    end

    # A refused feed means the URL was changed without re-saving, which would
    # otherwise read as the internet being down.
    if !self.following
      text(0, 0, "re-save", rgb(230, 150, 60))
      text(0, 9, "to apply", rgb(120, 90, 50))
      return
    end

    var ages = self._ages()
    var age = ages[0]
    var any = ages[1]

    # Never had an answer, and nothing has failed yet: the first fetch is
    # still on its way. Not an error, and not offline.
    if any < 0 && http_error(self.U1) == nil
      text(10, 5, "CHECK", rgb(110, 110, 120))
      return
    end

    if (age < 0 || age > self.graceMs) && any >= 0 && any <= self.graceMs
      self._tick(46, 1, rgb(190, 160, 60))
      if self.ip == nil
        text(0, 0, "no addr", rgb(190, 160, 60))
        text(0, 9, "in reply", rgb(110, 95, 60))
      else
        # The last address we did read, dimmed, because it is no longer being
        # confirmed even though the connection is fine.
        self._address(0, true)
      end
      return
    end

    # Offline: no good answer for longer than the missed-check budget allows.
    if age < 0 || age > self.graceMs
      var red = rgb(230, 70, 60)
      self._cross(0, 1, red)
      text(8, 0, "OFFLINE", red)

      if self.ip == nil
        text(8, 9, "no address", rgb(90, 60, 60))
      else
        # How long it has been wrong is more use here than an address that
        # stopped being true at some point in the past.
        var since = "for " + self._ago(age)
        if age < 0
          since = "never up"
        end
        text(8, 9, since, rgb(110, 80, 75))
      end
      return
    end

    # Online, with the address on two lines and a tick that does not move.
    self._tick(46, 1, rgb(80, 200, 100))
    self._address(0, false)
  end
end

return App()
