# name: Dice
# summary: Two dice tumble and land. Press the button to roll again - or wait, and they roll themselves.
# author: Stipple
# tags: game, interactive, random
# panel: 52x16

import math

class App
  var d1
  var d2
  var rolling
  var last
  var rested

  def init()
	self.d1 = 1 + math.rand() % 6
	self.d2 = 1 + math.rand() % 6
	self.rolling = 0
	self.last = now_ms()
	self.rested = now_ms()
  end

  def roll()
	self.rolling = 16
	self.rested = now_ms()
  end

  def on_button(name)
	if name != "select"
	  return
	end
	self.roll()
  end

  def die(x, y, face, col)
	rect(x, y, 12, 12, col)
	var pips = [[4], [0, 8], [0, 4, 8], [0, 2, 6, 8], [0, 2, 4, 6, 8], [0, 2, 3, 5, 6, 8]]
	for p : pips[face - 1]
	  rect_fill(x + 2 + (p % 3) * 3, y + 2 + (p / 3) * 3, 2, 2, col)
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 60
	  self.last = t
	  if self.rolling > 0
		self.rolling -= 1
		self.d1 = 1 + math.rand() % 6
		self.d2 = 1 + math.rand() % 6
		if self.rolling == 0 && audio_known()
		  sound("tock")
		end
	  elif t - self.rested > 7000
		self.roll()
	  end
	end
	clear(rgb(0, 20, 8))
	var col = self.rolling > 0 ? rgb(200, 200, 90) : rgb(255, 255, 255)
	self.die(9, 2, self.d1, col)
	self.die(31, 2, self.d2, col)
  end
end

return App()
