# name: Canyon Flyer
# summary: Thread a ship through a twisting neon canyon. The knob and -/+ steer up and down; grab the gems, and do not touch the walls.
# author: Stipple
# tags: game, flyer, interactive, landscape
# panel: 52x16

# @input exclusive

import math
import string

# The canyon is two lists, one number per column: how many rows of wall hang
# from the top and how many rise from the bottom. Each new column wanders a
# little from the last and the gap slowly narrows, so it starts forgiving and
# stops being.
class App
  var top, bot, gem
  var centre, gapSize
  var y, dist, gems, best
  var dead, last, hue

  def init()
	self.best = 0
	self._reset()
  end

  def _reset()
	self.top = []
	self.bot = []
	self.gem = []
	self.centre = 8.0
	self.gapSize = 11.0
	for i : 0 .. width() - 1
	  self.top.push(2)
	  self.bot.push(3)
	  self.gem.push(-1)
	end
	self.y = 8.0
	self.dist = 0
	self.gems = 0
	self.dead = false
	self.last = nil
	self.hue = 0
  end

  def _column()
	var drift = (math.rand() % 5 - 2) * 0.5
	self.centre += drift
	self.gapSize -= 0.004
	if self.gapSize < 6.0 self.gapSize = 6.0 end
	var half = self.gapSize / 2.0
	if self.centre < half + 1 self.centre = half + 1 end
	if self.centre > height() - half - 1 self.centre = height() - half - 1 end
	var t = int(self.centre - half)
	var b = height() - int(self.centre + half)
	self.top.remove(0)
	self.bot.remove(0)
	self.gem.remove(0)
	self.top.push(t)
	self.bot.push(b)
	self.gem.push(math.rand() % 14 == 0 ? int(self.centre) : -1)
  end

  def on_button(name)
	if self.dead
	  if name == 'select' self._reset() end
	  return
	end
	if name == 'left'
	  self.y -= 1.0
	elif name == 'right'
	  self.y += 1.0
	elif name == 'plus'
	  self.y -= 2.0
	elif name == 'minus'
	  self.y += 2.0
	end
  end

  def _tick()
	self._column()
	self.dist += 1
	self.hue = (self.hue + 1) % 256
	var sx = 9
	var py = int(self.y + 0.5)
	if py <= self.top[sx] - 1 || py >= height() - self.bot[sx]
	  self.dead = true
	  if self.dist > self.best self.best = self.dist end
	  tone(130, 250)
	  return
	end
	if self.gem[sx] >= 0 && math.abs(self.gem[sx] - py) <= 1
	  self.gem[sx] = -1
	  self.gems += 1
	  self.dist += 10
	  tone(880, 60)
	end
  end

  def draw()
	var w = width()
	var h = height()
	var now = now_ms()
	if self.last == nil self.last = now end
	if now - self.last >= 50
	  self.last = now
	  if !self.dead self._tick() end
	end

	if self.dead
	  text(4, 1, 'GAME', rgb(255, 255, 255))
	  text(4, 9, 'OVER', rgb(255, 90, 90))
	  text(30, 1, string.format('%d', self.dist), rgb(255, 220, 160))
	  if (now / 1500) % 2 == 0
		text(30, 9, string.format('B%d', self.best), rgb(255, 200, 120))
	  else
		text(30, 9, 'PUSH', rgb(120, 200, 255))
	  end
	  return
	end

	# Haze in the gap, brighter towards the middle, like fog lit from below.
	for y : 0 .. h - 1
	  var d = math.abs(y - 7.5) / 7.5
	  rect_fill(0, y, w, 1, rgb(int(10 + 14 * (1 - d)), 6, int(30 + 30 * (1 - d))))
	end

	for x : 0 .. w - 1
	  var t = self.top[x]
	  var b = self.bot[x]
	  var glow = 90 + int(70 * math.sin((x + self.hue) / 6.0))
	  if t > 0
		rect_fill(x, 0, 1, t, rgb(40, 18, 70))
		pixel(x, t - 1, rgb(glow + 100, 60, 200))
	  end
	  if b > 0
		rect_fill(x, h - b, 1, b, rgb(40, 18, 70))
		pixel(x, h - b, rgb(60, glow + 100, 220))
	  end
	  var g = self.gem[x]
	  if g >= 0
		pixel(x, g, (now / 150 + x) % 2 == 0 ? rgb(255, 240, 120) : rgb(255, 150, 60))
	  end
	end

	# The ship and its engine trail.
	var py = int(self.y + 0.5)
	pixel(9, py, rgb(255, 255, 255))
	pixel(10, py, rgb(140, 220, 255))
	pixel(8, py, rgb(255, 160, 60))
	pixel(7, py, rgb(160, 60, 30))

	var s = string.format('%d', self.dist)
	text(w - text_width(s), 0, s, rgb(255, 220, 160))
  end
end

return App()
