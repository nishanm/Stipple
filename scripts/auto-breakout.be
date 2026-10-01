# name: Auto Breakout
# summary: A self-playing game of Breakout. The paddle tracks the ball, sometimes badly, and the wall rebuilds when it is cleared. Press to rebuild the wall.
# author: Stipple
# tags: game, animation, ambient, button
# panel: 52x16

# The bricks are a 12 by 4 grid drawn one pixel tall with a gap between rows.
# The ball moves less than a pixel per step so a brick test on its current cell
# is enough. The paddle aims at where the ball will land plus an error picked
# at each bounce, which is why it occasionally misses and has to serve again.

import math

class App
  var bricks, left
  var bx, by, bvx, bvy
  var pdx, err, wait
  var last, tk, flash

  def init()
	self.bricks = []
	self.last = 0
	self.tk = 0
	self.pdx = 26.0
	self.err = 0.0
	self.flash = 0
	self.build()
	self.serve()
  end

  def build()
	self.bricks = []
	for i : 0 .. 47
	  self.bricks.push(true)
	end
	self.left = 48
  end

  def serve()
	self.bx = self.pdx
	self.by = 13.0
	var s = (math.rand() % 2 == 0) ? 1.0 : -1.0
	self.bvx = s * (0.25 + (math.rand() % 20) / 100.0)
	self.bvy = -0.4
	self.wait = 30
  end

  def on_button(name)
	self.build()
	self.serve()
  end

  def step()
	self.tk += 1
	if self.flash > 0
	  self.flash -= 1
	end

	var target = self.bx + self.err
	if self.bvy < 0
	  target = 26.0 + (self.bx - 26.0) * 0.5
	end
	var d = target - self.pdx
	if d > 0.9
	  d = 0.9
	elif d < -0.9
	  d = -0.9
	end
	self.pdx += d
	if self.pdx < 4.0
	  self.pdx = 4.0
	elif self.pdx > 47.0
	  self.pdx = 47.0
	end

	if self.wait > 0
	  self.wait -= 1
	  self.bx = self.pdx
	  return nil
	end

	self.bx += self.bvx
	self.by += self.bvy

	if self.bx < 0.0
	  self.bx = 0.0
	  self.bvx = math.abs(self.bvx)
	elif self.bx > 51.0
	  self.bx = 51.0
	  self.bvx = 0.0 - math.abs(self.bvx)
	end
	if self.by < 0.0
	  self.by = 0.0
	  self.bvy = math.abs(self.bvy)
	end

	var ix = int(self.bx)
	var iy = int(self.by)
	if iy >= 1 && iy <= 8 && ix >= 2 && ix <= 49
	  var col = (ix - 2) / 4
	  var row = (iy - 1) / 2
	  var k = row * 12 + col
	  if self.bricks[k]
		self.bricks[k] = false
		self.left -= 1
		self.bvy = math.abs(self.bvy)
		if self.left == 0
		  self.flash = 20
		  self.build()
		end
	  end
	end

	if self.bvy > 0 && self.by >= 14.0 && self.by < 15.0 && math.abs(self.bx - self.pdx) <= 4.5
	  self.bvy = 0.0 - math.abs(self.bvy)
	  var v = (self.bx - self.pdx) / 4.0 * 0.6
	  if math.abs(v) < 0.15
		v = v < 0 ? -0.15 : 0.15
	  end
	  self.bvx = v
	  self.err = ((math.rand() % 100) - 50) / 12.0
	end

	if self.by >= 16.0
	  self.serve()
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.step()
	end

	clear(self.flash > 0 && self.flash % 4 < 2 ? 0x303030 : 0x000000)
	var colors = [0xE03030, 0xE0A020, 0x30C040, 0x3070E0]
	for r : 0 .. 3
	  for c : 0 .. 11
		if self.bricks[r * 12 + c]
		  rect_fill(2 + c * 4, 1 + r * 2, 3, 1, colors[r])
		end
	  end
	end
	rect_fill(int(self.pdx) - 4, 15, 9, 1, 0xC0C8D8)
	pixel(int(self.bx), int(self.by), 0xFFFFFF)
  end
end

return App()
