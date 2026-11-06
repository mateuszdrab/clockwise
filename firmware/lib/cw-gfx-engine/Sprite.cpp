
#include "Sprite.h"
#include <SystemLog.h>

boolean Sprite::collidedWith(Sprite* sprite) {
  return (this->_x < sprite->_x + sprite->_width &&
          this->_x + this->_width > sprite->_x &&
          this->_y < sprite->_y + sprite->_height &&
          this->_y + this->_height > sprite->_y);
}

void Sprite::logPosition() {
  SerialLog::instance().print("x = "); SerialLog::instance().print(_x); SerialLog::instance().print(", y = "); SerialLog::instance().print(_y);
  SerialLog::instance().print(", w = "); SerialLog::instance().print(_width); SerialLog::instance().print(", h = "); SerialLog::instance().println(_height);
}
