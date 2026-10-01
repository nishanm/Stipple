# name: Heartbeat
# summary: A hospital-monitor ECG trace with a pulsing heart. The rate wanders gently, as a real one does.
# author: Stipple
# tags: animation, retro, medical
# panel: 52x16

import math
import string

class App
  var trace
  var pos
  var last
  var bpm
  var beat

  def init()
	self.trace = []
	for i : 0 .. 39
	  self.trace.push(0)
	end
	self.pos = 0
	self.bpm = 72
	self.beat = 0
	self.last = now_ms()
  end

  def draw()
	var pattern = [0, 0, 0, 0, 1, 2, 1, 0, 0, -1, 3, 8, -4, -2, 0, 0, 1, 1, 2, 2, 1, 0, 0, 0, 0, 0]
	var t = now_ms()
	if t - self.last >= 45
	  self.last = t
	  var v = pattern[self.pos]
	  self.pos = (self.pos + 1) % pattern.size()
	  if self.pos == 0
		self.bpm = 66 + int(math.sin(t / 4000.0) * 8) + 8
	  end
	  if v == 8
		self.beat = 6
	  end
	  self.trace.push(v)
	  self.trace.pop(0)
	  if self.beat > 0
		self.beat -= 1
	  end
	end

	clear(rgb(0, 4, 2))
	var n = self.trace.size()
	var prev = 8 - self.trace[0]
	for i : 1 .. n - 1
	  var y = 8 - self.trace[i]
	  var g = 60 + i * 5
	  line(11 + i - 1, prev, 11 + i, y, rgb(0, g, g / 3))
	  prev = y
	end
	# The bright leading dot.
	pixel(11 + n - 1, prev, rgb(200, 255, 220))

	var big = self.beat > 3
	var heart = big ? rgb(255, 40, 70) : rgb(170, 20, 45)
	var hx = 1
	var hy = 1
	rect_fill(hx, hy, 2, 2, heart)
	rect_fill(hx + 3, hy, 2, 2, heart)
	rect_fill(hx, hy + 1, 5, 2, heart)
	rect_fill(hx + 1, hy + 3, 3, 1, heart)
	pixel(hx + 2, hy + 4, heart)
	text(1, 9, string.format("%d", self.bpm), rgb(0, 200, 90))
  end
end

return App()
