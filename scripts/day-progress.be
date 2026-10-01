# name: Day Progress
# summary: How much of the day has gone, as a bar you can read from across a room.
# author: Stipple
# tags: time, ambient
# panel: 52x16

import string

class App
  def draw()
    clear(rgb(0, 0, 0))

    if !time_known()
      text(8, 5, "no time", rgb(180, 60, 60))
      return
    end

    var minutes = hour() * 60 + minute()
    var through = (minutes * width()) / 1440

    # "14:37", five characters: 1 to 31.
    text(1, 0, string.format("%02d:%02d", hour(), minute()), rgb(210, 210, 210))

    var left = 1440 - minutes
    var remaining = string.format("%2dh%02d", left / 60, left % 60)
    text(width() - text_width(remaining), 0, remaining, rgb(80, 80, 80))

    var evening = (minutes * 255) / 1440
    var colour = rgb(255 - evening, 90 + evening / 3, evening)

    rect(0, 10, width(), 5, rgb(22, 22, 22))
    if through > 1
      rect_fill(1, 11, through - 1, 3, colour)
    end
  end
end

return App()
