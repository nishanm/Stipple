# name: Auto Pong
# summary: Two computer players rally forever. Nobody wins for long.
# author: Stipple
# tags: game, retro, animation
# panel: 52x16

import math
import string

class App
  var bx
  var by
  var dx
  var dy
  var left
  var right
  var scoreL
  var scoreR
  var last

  def init()
	self.scoreL = 0
	self.scoreR = 0
	self.last = now_ms()
	self.serve(1)
  end

  def serve(dir)
	self.bx = width() / 2
	self.by = 4 + (math.rand() % 8)
	self.dx = dir
	self.dy = (math.rand() % 2) == 0 ? 1 : -1
	self.left = 6
	self.right = 6
  end

  # Paddle is 4 tall; nudge it toward the ball, but not always - a perfect
  # player would make the game a screensaver of a single line.
  def follow(paddle, target, miss)
	if miss
	  return paddle
	end
	if target < paddle + 1
	  paddle = paddle - 1
	elif target > paddle + 2
	  paddle = paddle + 1
	end
	if paddle < 0
	  paddle = 0
	end
	if paddle > height() - 4
	  paddle = height() - 4
	end
	return paddle
  end

  def step()
	var w = width()
	var h = height()

	self.bx = self.bx + self.dx
	self.by = self.by + self.dy

	if self.by <= 0
	  self.by = 0
	  self.dy = 1
	elif self.by >= h - 1
	  self.by = h - 1
	  self.dy = -1
	end

	# Only track when the ball is heading their way.
	if self.dx < 0
	  self.left = self.follow(self.left, self.by, (math.rand() % 6) == 0)
	else
	  self.right = self.follow(self.right, self.by, (math.rand() % 6) == 0)
	end

	if self.bx <= 2
	  if self.by >= self.left && self.by <= self.left + 3
		self.dx = 1
		self.bx = 2
	  else
		self.scoreR = (self.scoreR + 1) % 10
		self.serve(1)
	  end
	elif self.bx >= w - 3
	  if self.by >= self.right && self.by <= self.right + 3
		self.dx = -1
		self.bx = w - 3
	  else
		self.scoreL = (self.scoreL + 1) % 10
		self.serve(-1)
	  end
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 70
	  self.last = t
	  self.step()
	end

	clear(rgb(0, 0, 0))

	# Dotted centre net.
	for y : 0 .. height() - 1
	  if y % 2 == 0
		pixel(width() / 2, y, rgb(50, 50, 50))
	  end
	end

	var white = rgb(230, 230, 230)
	text(width() / 2 - 10, 0, string.format("%d", self.scoreL), rgb(90, 90, 90))
	text(width() / 2 + 6, 0, string.format("%d", self.scoreR), rgb(90, 90, 90))

	rect_fill(1, self.left, 1, 4, rgb(80, 200, 255))
	rect_fill(width() - 2, self.right, 1, 4, rgb(255, 130, 80))
	pixel(self.bx, self.by, white)
  end
end

return App()
