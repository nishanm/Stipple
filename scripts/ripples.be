# name: Ripples
# summary: Raindrops land on dark water and send out expanding, fading rings.
# author: Stipple
# tags: ambient, animation, relaxing, water
# panel: 52x16

import math

class App
  var cx
  var cy
  var age
  var last

  def init()
	self.cx = []
	self.cy = []
	self.age = []
	self.last = now_ms()
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 50
	  self.last = t
	  for i : 0 .. self.age.size() - 1
		self.age[i] = self.age[i] + 1
	  end
	  # Drop finished ripples, oldest first.
	  while self.age.size() > 0 && self.age[0] > 22
		self.cx.pop(0)
		self.cy.pop(0)
		self.age.pop(0)
	  end
	  if self.age.size() < 6 && (math.rand() % 7) == 0
		self.cx.push(math.rand() % width())
		self.cy.push(math.rand() % height())
		self.age.push(0)
	  end
	end

	clear(rgb(0, 6, 22))

	for i : 0 .. self.age.size() - 1
	  var a = self.age[i]
	  var fade = 255 - a * 11
	  if fade < 0
		fade = 0
	  end
	  # Leading ring, and a dimmer one following behind it.
	  for ring : 0 .. 1
		var r = a - ring * 4
		if r > 0
		  var v = ring == 0 ? fade : fade / 3
		  for s : 0 .. 31
			var ang = s * 0.19635
			var px = self.cx[i] + int(math.cos(ang) * r * 1.6)
			var py = self.cy[i] + int(math.sin(ang) * r * 0.8)
			pixel(px, py, rgb(v / 4, v * 2 / 3, v))
		  end
		end
	  end
	end
  end
end

return App()
