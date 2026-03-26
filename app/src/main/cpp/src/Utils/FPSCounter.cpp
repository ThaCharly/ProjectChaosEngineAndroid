#include "FPSCounter.hpp"
#include <sstream>

FPSCounter::FPSCounter(const sf::Font& font, unsigned int characterSize, sf::Color color)
{
    m_text.setFont(font);
    m_text.setCharacterSize(characterSize);
    m_text.setFillColor(color);
    m_text.setString("0 FPS");
}

void FPSCounter::update(float dt, const sf::RenderWindow& window)
{
    m_elapsed += dt;
    ++m_frames;
    if (m_elapsed >= 0.5f) {
        m_fps = static_cast<int>(m_frames / m_elapsed + 0.5f);
        m_frames = 0;
        m_elapsed = 0.f;
        std::ostringstream ss;
        ss << m_fps << " FPS";
        m_text.setString(ss.str());
    }

    sf::FloatRect bounds = m_text.getLocalBounds();
    float x = static_cast<float>(window.getSize().x) - bounds.width - bounds.left - 8.f;
    float y = 8.f;
    m_text.setPosition(x, y);
}

void FPSCounter::draw(sf::RenderTarget& target) const
{
    target.draw(m_text);
}
