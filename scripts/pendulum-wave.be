# name: Pendulum Wave
# summary: Sixteen pendulums of slightly different lengths drift in and out of step, making snakes, waves and chaos.
# author: Stipple
# tags: animation, physics, hypnotic
# panel: 52x16

import math

class App
  def wheel(p)
	var a = p * 0.0245
	return rgb(128 + int(127 * math.sin(a)), 128 + int(127 * math.sin(a + 2.094)), 128 + int(127 * math.sin(a + 4.188)))
  end

  def draw()
	clear(rgb(0, 0, 8))
	# Pendulum i makes (20 + i) swings in 40 seconds, so all of them line up
	# again at the start of every cycle - which is the whole trick.
	var t = elapsed_ms() / 1000.0
	for i : 0 .. 15
	  var x = 2 + i * 3
	  var y = 8 + int(math.sin(2.0 * math.pi * (20 + i) / 40.0 * t) * 6)
	  line(x, 0, x, y, rgb(20, 20, 40))
	  var c = self.wheel(i * 48)
	  rect_fill(x - 1, y - 1, 3, 3, c)
	end
  end
end

return App()
