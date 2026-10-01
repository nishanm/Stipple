# name: Oscilloscope
# summary: Two drifting sine waves on a glowing green phosphor grid, with a slow trace fade.
# author: Stipple
# tags: animation, retro, ambient
# panel: 52x16

import math

class App
  var phase
  var last

  def init()
	self.phase = 0.0
	self.last = now_ms()
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 40
	  self.last = t
	  self.phase = self.phase + 0.25
	end

	clear(rgb(0, 12, 4))

	# Graticule: faint dots every four pixels, brighter centre line.
	var grid = rgb(0, 40, 14)
	for gx : 0 .. (width() / 4)
	  for gy : 0 .. (height() / 4)
		pixel(gx * 4, gy * 4, grid)
	  end
	end
	line(0, 8, width() - 1, 8, rgb(0, 60, 22))

	var prev = -1
	for x : 0 .. width() - 1
	  var amp = 4.0 + math.sin(self.phase * 0.2) * 2.5
	  var v = math.sin(x / 4.0 + self.phase) * amp
	  var y = 8 + int(v)
	  if y < 0
		y = 0
	  end
	  if y > height() - 1
		y = height() - 1
	  end
	  if prev >= 0
		line(x - 1, prev, x, y, rgb(40, 255, 110))
	  end
	  pixel(x, y, rgb(190, 255, 210))
	  prev = y
	end
  end
end

return App()
