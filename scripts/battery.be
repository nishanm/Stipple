# name: Battery
# summary: Charge as a gauge, and nothing at all when there is no battery to report.
# author: Stipple
# tags: status
# panel: 52x16

import string

class App
  def draw()
    clear(rgb(0, 0, 0))

    if !battery_known()
      text(4, 5, "no battery", rgb(120, 120, 120))
      return
    end

    var percent = battery()

    # Red under a fifth, amber under a half, green above. The thresholds are
    # where somebody's behaviour changes, not evenly spaced.
    var colour = rgb(0, 200, 80)
    if percent < 20
      colour = rgb(240, 60, 60)
    elif percent < 50
      colour = rgb(240, 170, 0)
    end

    # A battery outline, drawn as one: the shape says what the number means
    # without needing a label for it.
    rect(0, 3, 34, 10, rgb(70, 70, 70))
    rect_fill(34, 6, 2, 4, rgb(70, 70, 70))

    var filled = (percent * 30) / 100
    if filled > 0
      rect_fill(2, 5, filled, 6, colour)
    end

    # The number itself, right-aligned so 9, 64 and 100 all end in the same
    # place instead of drifting.
    var label = string.format("%d", percent)
    text(width() - text_width(label), 4, label, rgb(170, 170, 170))

    if charging()
      # A bolt would be four pixels of mush at this size. A block that blinks
      # says the same thing and survives the resolution.
      if (now_ms() / 500) % 2 == 0
        rect_fill(15, 6, 4, 4, rgb(255, 255, 255))
      end
    end
  end
end

return App()
