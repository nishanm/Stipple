# name: New Year Countdown
# summary: Days until the next 1 January - and on 31 December, the last hours, minutes and seconds, with sparks.
# author: Stipple
# tags: clock, countdown, holiday
# panel: 52x16

import math
import string

class App
  def leap(y)
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0
  end

  def dayOfYear()
	var m = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
	if self.leap(year())
	  m[1] = 29
	end
	var n = day()
	for i : 0 .. month() - 2
	  n += m[i]
	end
	return n
  end

  def sparks(n)
	for i : 0 .. n
	  var c = math.rand() % 3
	  var col = c == 0 ? rgb(255, 210, 60) : (c == 1 ? rgb(255, 90, 90) : rgb(90, 200, 255))
	  pixel(math.rand() % 52, math.rand() % 16, col)
	end
  end

  def draw()
	clear(rgb(0, 0, 8))
	if !time_known()
	  text(8, 5, "no time", rgb(180, 60, 60))
	  return
	end

	var doy = self.dayOfYear()
	var total = self.leap(year()) ? 366 : 365
	var left = total - doy

	if doy == 1
	  text(4, 1, "HAPPY", rgb(255, 210, 60))
	  text(2, 9, "NEW YEAR", rgb(255, 90, 90))
	  self.sparks(14)
	elif left == 0
	  var secs = (23 - hour()) * 3600 + (59 - minute()) * 60 + (60 - second())
	  var h = secs / 3600
	  var m = (secs % 3600) / 60
	  var s = secs % 60
	  text(1, 0, string.format("%d", year() + 1), rgb(255, 210, 60))
	  text(1, 9, string.format("%02d:%02d:%02d", h, m, s), rgb(255, 255, 255))
	  if secs < 60
		self.sparks(10)
	  end
	else
	  var num = string.format("%d", left)
	  var w = text_width(num)
	  text((52 - w) / 2, 1, num, rgb(90, 200, 255))
	  var label = left == 1 ? "DAY TO" : "DAYS TO"
	  text((52 - text_width(label)) / 2, 9, label, rgb(120, 120, 150))
	end
  end
end

return App()
