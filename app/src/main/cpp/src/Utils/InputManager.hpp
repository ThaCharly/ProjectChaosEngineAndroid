#pragma once
#include <SFML/Window/Event.hpp>
#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Window/Mouse.hpp>
#include <SFML/Window/Touch.hpp>


enum class PointerState {
    Idle,
    Pressed,  // Primer frame que se tocó/hizo clic
    Held,     // Se mantiene apretado
    Released  // Frame en que se soltó
};

struct InputState {
    PointerState state = PointerState::Idle;
    sf::Vector2f position; // Coordenadas de la ventana
    sf::Vector2f delta;    // Cuánto se movió desde el último frame (ideal para el Drag)
    bool handledByUI = false; // ImGui se morfó el input?
};

class InputManager {
private:
    InputState pointer;
    sf::Vector2f lastPos;

public:
    void update(const sf::RenderWindow& window, bool uiWantsCapture) {
        pointer.handledByUI = uiWantsCapture;

        // Soporte Híbrido: Priorizamos Touch (Android), fallback a Mouse (PC)
        bool isDown = sf::Touch::isDown(0) || sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        
        sf::Vector2i currentPosInt;
        if (sf::Touch::isDown(0)) {
            currentPosInt = sf::Touch::getPosition(0, window);
        } else {
            currentPosInt = sf::Mouse::getPosition(window);
        }
        
        sf::Vector2f currentPos(currentPosInt.x, currentPosInt.y);
        pointer.delta = currentPos - lastPos;
        pointer.position = currentPos;
        lastPos = currentPos;

        // Máquina de estados del puntero
        if (isDown) {
            if (pointer.state == PointerState::Idle || pointer.state == PointerState::Released)
                pointer.state = PointerState::Pressed;
            else
                pointer.state = PointerState::Held;
        } else {
            if (pointer.state == PointerState::Pressed || pointer.state == PointerState::Held)
                pointer.state = PointerState::Released;
            else
                pointer.state = PointerState::Idle;
        }
    }

    const InputState& getPointer() const { return pointer; }
};