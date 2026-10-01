# name: GitHub
# summary: A year of contributions as the real heatmap, 52 weeks across the panel.
# author: Stipple
# tags: http, github, graph, developer
# panel: 52x16

# @config user text "GitHub username" default="galadril" maxlen=39 help="Just the name, not the full profile URL"
# @config total boolean "Show the year's total" default=true

import string

class App
  var URL, who
  var levels          # 0..4 per day, index 0 = January 1st
  var total
  var lastAge
  var DAYS            # cumulative days before each month, non-leap

  def init()
    self.levels = nil
    self.total = -1
    self.lastAge = -1
    self.DAYS = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]
    self._point()
  end

  def _point()
    self.who = store.get("user", "galadril")

    var y = time_known() ? year() : 2026
    self.URL = "https://github-contributions-api.jogruber.de/v4/" +
               self.who + "?y=" + str(y)
  end

  def duration()
    return 9000
  end

  def _leap(y)
    if y % 400 == 0
      return true
    end
    if y % 100 == 0
      return false
    end
    return y % 4 == 0
  end

  # 1st January is day 0.
  def _dayOfYear()
    var d = self.DAYS[month() - 1] + day() - 1
    if month() > 2 && self._leap(year())
      d += 1
    end
    return d
  end

  def _parse()
    var body = http_get(self.URL)
    if body == nil
      return
    end

    # One C call. The first piece is everything before the first level and is
    # thrown away; each of the rest begins with the digit.
    var parts = string.split(body, '"level":')
    var n = size(parts)
    if n < 2
      return
    end

    var out = []
    var i = 1
    while i < n
      var c = parts[i][0]
      var v = 0
      if c == "1"
        v = 1
      elif c == "2"
        v = 2
      elif c == "3"
        v = 3
      elif c == "4"
        v = 4
      end
      out.push(v)
      i += 1
    end
    self.levels = out

    # "lastYear" is the headline number GitHub itself shows.
    var mark = string.find(body, '"lastYear":')
    if mark >= 0
      var tail = body[mark + 11 .. mark + 20]
      var num = 0
      var k = 0
      var seen = false
      while k < size(tail)
        var ch = tail[k]
        if ch >= "0" && ch <= "9"
          num = num * 10 + int(ch)
          seen = true
        else
          break
        end
        k += 1
      end
      if seen
        self.total = num
      end
    end
  end

  def draw()
    clear(rgb(0, 0, 0))
    self._point()
    http_follow(self.URL, 1800)

    if !http_known()
      text(2, 1, "no net", rgb(150, 60, 60))
      return
    end

    # Parse when a new body lands. The age resets to near zero on a fresh
    # fetch, so a drop is the signal - and it is the only one a script gets.
    var age = http_age_ms(self.URL)
    if age >= 0 && (self.levels == nil || age < self.lastAge)
      self._parse()
    end
    if age >= 0
      self.lastAge = age
    end

    if self.levels == nil
      var why = http_error(self.URL)
      if why != nil
        text(1, 1, "err", rgb(200, 70, 50))
        text(1, 9, why, rgb(90, 60, 60))
      elif http_status(self.URL) == 404
        text(1, 1, "no user", rgb(200, 70, 50))
      else
        text(1, 5, "fetching", rgb(70, 70, 70))
      end
      return
    end

    self._grid()

    if store.get("total", true) && self.total >= 0
      text(0, 9, str(self.total), rgb(200, 210, 220))
    else
      text(0, 9, self.who, rgb(90, 100, 115))
    end
  end

  def _grid()

    var shades = [rgb(14, 18, 24), rgb(10, 52, 34), rgb(0, 86, 40),
                  rgb(32, 128, 52), rgb(60, 200, 90)]

    var today = time_known() ? self._dayOfYear() : size(self.levels) - 1
    if today >= size(self.levels)
      today = size(self.levels) - 1
    end

    # The rightmost column is this week, and today sits on its own weekday
    # row - so the graph ends where the eye expects it to.
    var row = time_known() ? weekday() : 6

    var index = today
    var x = 51
    var y = row

    while x >= 0 && index >= 0
      var level = self.levels[index]

      pixel(x, y, shades[level])

      index -= 1
      y -= 1
      if y < 0
        y = 6
        x -= 1
      end
    end
  end
end

return App()
