#pragma once
#include <SFML/Graphics.hpp>

class FPSCounter {
public:
    FPSCounter(const sf::Font& font, unsigned int characterSize = 16, sf::Color color = sf::Color::White);
    void update(float dt, const sf::RenderWindow& window);
    void draw(sf::RenderTarget& target) const;

private:
    sf::Text m_text;
    float m_elapsed = 0.f;
    int m_frames = 0;
    int m_fps = 0;
};
