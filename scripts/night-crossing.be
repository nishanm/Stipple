# name: Night Crossing
# summary: Get the frog across the road and the river. The knob hops sideways, + hops forward, - hops back. Ride the logs, dodge the cars.
# author: Stipple
# tags: game, arcade, interactive
# panel: 52x16

# @input exclusive

import math
import string

# Nothing about the traffic is stored. Every lane is a repeating pattern that
# is a pure function of the lane and the clock, so a car's place is worked
# out when it is drawn and when the frog is tested against it. The pattern
# repeats every 60 columns, wider than the panel, which is what lets objects
# slide on and off the edge without a spawn step.
class App
  var fx, fy
  var t, lives, score, level, best
  var dead, last, flash, home

  def init()
	self.best = 0
	self._reset()
  end

  def _reset()
	self.lives = 3
	self.score = 0
	self.level = 1
	self.dead = false
	self.home = 0
	self.t = 0.0
	self.last = nil
	self.flash = 0
	self._spawn()
  end

  def _spawn()
	self.fx = 26.0
	self.fy = 14
  end

  # speed in columns a second, spacing between objects, length.
  def _lane(row)
	var s = 1.0 + self.level * 0.15
	if row >= 7 && row <= 13
	  var i = row - 7
	  var dir = i % 2 == 0 ? 1.0 : -1.0
	  var sp = (3.0 + (i * 7 % 4) * 1.5) * (s / 1.15)
	  var space = i % 3 == 0 ? 20 : (i % 3 == 1 ? 15 : 12)
	  return [dir * sp, space, i % 3 == 0 ? 4 : 2]
	end
	if row >= 1 && row <= 5
	  var i = row - 1
	  var dir = i % 2 == 0 ? -1.0 : 1.0
	  var sp = (2.5 + (i * 5 % 3) * 1.5) * (s / 1.15)
	  var space = i % 2 == 0 ? 15 : 12
	  return [dir * sp, space, i % 2 == 0 ? 7 : 5]
	end
	return nil
  end

  def _pos(lane, k)
	var raw = int(k * lane[1] + lane[0] * self.t)
	return ((raw % 60) + 60) % 60 - 6
  end

  def _covered(row, x)
	var lane = self._lane(row)
	if lane == nil return false end
	var n = 60 / lane[1]
	for k : 0 .. n - 1
	  var p = self._pos(lane, k)
	  if x >= p && x <= p + lane[2] - 1 return true end
	end
	return false
  end

  def _die()
	self.lives -= 1
	self.flash = 10
	tone(120, 250)
	if self.lives <= 0
	  self.dead = true
	  if self.score > self.best self.best = self.score end
	else
	  self._spawn()
	end
  end

  def _check()
	var x = int(self.fx + 0.5)
	if self.fy >= 7 && self.fy <= 13
	  if self._covered(self.fy, x) self._die() end
	elif self.fy >= 1 && self.fy <= 5
	  if !self._covered(self.fy, x) self._die() end
	end
  end

  def on_button(name)
	if self.dead
	  if name == 'select' self._reset() end
	  return
	end
	if name == 'left'
	  self.fx -= 2
	elif name == 'right'
	  self.fx += 2
	elif name == 'plus'
	  self.fy -= 1
	  self.score += 1
	  tone(500, 15)
	elif name == 'minus'
	  if self.fy < 14 self.fy += 1 end
	end
	if self.fx < 1 self.fx = 1.0 end
	if self.fx > width() - 2 self.fx = width() - 2.0 end
	if self.fy <= 0
	  self.score += 50 * self.level
	  self.home += 1
	  tone(900, 100)
	  if self.home % 3 == 0 self.level += 1 end
	  self._spawn()
	else
	  self._check()
	end
  end

  def _tick(dt)
	self.t += dt
	if self.flash > 0 self.flash -= 1 end
	if self.fy >= 1 && self.fy <= 5
	  # Carried along by the log underfoot.
	  var lane = self._lane(self.fy)
	  self.fx += lane[0] * dt
	  if self.fx < 0 || self.fx > width() - 1
		self._die()
		return
	  end
	end
	self._check()
  end

  def draw()
	var w = width()
	var now = now_ms()
	if self.last == nil self.last = now end
	var ms = now - self.last
	if ms >= 50
	  self.last = now
	  if ms > 200 ms = 200 end
	  if !self.dead self._tick(ms / 1000.0) end
	end

	if self.dead
	  text(4, 1, 'GAME', rgb(255, 255, 255))
	  text(4, 9, 'OVER', rgb(255, 90, 90))
	  text(30, 1, string.format('%d', self.score), rgb(255, 220, 160))
	  if (now / 1500) % 2 == 0
		text(30, 9, string.format('B%d', self.best), rgb(255, 200, 120))
	  else
		text(30, 9, 'PUSH', rgb(120, 200, 255))
	  end
	  return
	end

	for row : 0 .. 15
	  if row == 0
		rect_fill(0, row, w, 1, rgb(20, 80, 40))
	  elif row <= 5
		rect_fill(0, row, w, 1, rgb(8, 30, 90 + row * 8))
		for k : 0 .. 5
		  var sx = (k * 11 + row * 7 + now / 300) % w
		  pixel(sx, row, rgb(30, 80, 160))
		end
	  elif row == 6 || row >= 14
		rect_fill(0, row, w, 1, rgb(24, 70, 36))
	  else
		rect_fill(0, row, w, 1, rgb(38, 38, 48))
		if row < 13 && row > 6
		  for x : 0 .. 12
			pixel(x * 4 + 1, row, rgb(50, 50, 60))
		  end
		end
	  end
	end

	for row : 1 .. 13
	  var lane = self._lane(row)
	  if lane != nil
		var n = 60 / lane[1]
		for k : 0 .. n - 1
		  var p = self._pos(lane, k)
		  if row >= 7
			var hue = (row * 3 + k) % 4
			var body = hue == 0 ? rgb(255, 80, 80) : (hue == 1 ? rgb(255, 210, 60) : (hue == 2 ? rgb(90, 190, 255) : rgb(230, 230, 240)))
			rect_fill(p, row, lane[2], 1, body)
			# Headlights on the leading end.
			var lead = lane[0] > 0 ? p + lane[2] - 1 : p
			pixel(lead, row, rgb(255, 255, 200))
		  else
			rect_fill(p, row, lane[2], 1, rgb(120, 76, 36))
			pixel(p, row, rgb(160, 110, 60))
		  end
		end
	  end
	end

	var fc = self.flash > 0 && self.flash % 2 == 0 ? rgb(255, 255, 255) : rgb(90, 255, 110)
	var x = int(self.fx + 0.5)
	pixel(x, self.fy, fc)
	pixel(x - 1, self.fy, rgb(30, 150, 60))
	pixel(x + 1, self.fy, rgb(30, 150, 60))

	for l : 1 .. self.lives
	  pixel(w - l * 2, 15, rgb(255, 70, 90))
	end
	var s = string.format('%d', self.score)
	text(w - text_width(s), 0, s, rgb(255, 220, 160))
  end
end

return App()
