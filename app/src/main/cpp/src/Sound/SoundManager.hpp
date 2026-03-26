#pragma once

#include <SFML/Audio.hpp>
#include <vector>
#include <map>
#include <cmath>
#include <iostream>
#include <random>
#include <optional>
#include <cstdint>

// Forward declaration
class Recorder;

class SoundManager {
public:
    SoundManager() {
        rng.seed(std::random_device{}());

        // --- CAMBIO: GENERAR LAS 128 NOTAS MIDI ---
        // Fórmula: f = 440 * 2^((d - 69) / 12)
        // Generamos del 0 al 127.
        for (int i = 0; i < 128; ++i) {
            float freq = 440.0f * std::pow(2.0f, (i - 69) / 12.0f);
            generateTone(i, freq);
        }

        // Pool de voces (aumenté un poco por si se pica la canción rápida)
        soundPool.resize(64); // Se llena de std::nullopt para inicializarlos On-Demand (SFML 3)
    }

    void setRecorder(Recorder* rec) {
        recorder = rec;
    }

    // Generador de ondas (Senoide suave)
    void generateTone(int id, float frequency) {
        const unsigned SAMPLE_RATE = 44100;
        const int AMPLITUDE = 18000;

        std::vector<std::int16_t> rawSamples;
        float duration = 0.3f; // Un poquito más cortas para melodías rápidas
        int numSamples = (int)(SAMPLE_RATE * duration);
        float attackTime = 0.01f; // Ataque rápido

        for (int i = 0; i < numSamples; i++) {
            float t = (float)i / SAMPLE_RATE;
            float wave = std::sin(2 * 3.14159f * frequency * t);

            float envelope = 0.0f;
            if (t < attackTime) {
                envelope = t / attackTime;
            } else {
                envelope = std::exp(-10.0f * (t - attackTime));
            }

            // Safety release para evitar "pops"
            float fadeOutStart = duration - 0.05f;
            if (t > fadeOutStart) {
                float fade = 1.0f - ((t - fadeOutStart) / 0.05f);
                if (fade < 0.0f) fade = 0.0f;
                envelope *= fade;
            }

            rawSamples.push_back((std::int16_t)(wave * envelope * AMPLITUDE));
        }

        sf::SoundBuffer buffer;
        // SFML 3 exige el Channel Map. Como es 1 canal, le pasamos {sf::SoundChannel::Mono}
        if (buffer.loadFromSamples(rawSamples.data(), rawSamples.size(), 1, SAMPLE_RATE, {sf::SoundChannel::Mono})) {
            midiBuffers[id] = buffer;
        }
    }

    // ESTA ES LA NUEVA FUNCIÓN CLAVE
    void playMidiNote(int noteNumber, float volume = 98.0f) {
        if (noteNumber < 0 || noteNumber > 127) return;
        if (midiBuffers.find(noteNumber) == midiBuffers.end()) return;

        for (auto& s : soundPool) {
            if (!s.has_value() || s->getStatus() == sf::Sound::Status::Stopped) {
                s.emplace(midiBuffers[noteNumber]); // Magia RAII: Construye el sonido in-place con su buffer
                s->setVolume(volume);
                s->setPitch(1.0f);
                s->setPosition({0.0f, 0.0f, 0.0f}); // Exige un vector estricto 3D
                s->setAttenuation(0.0f);
                s->play();
                break;
            }
        }

        if (recorder) {
            const sf::SoundBuffer& buf = midiBuffers[noteNumber];
            sendToRecorder(buf.getSamples(), buf.getSampleCount(), volume);
        }
    }

    // Mantenemos esta por compatibilidad con el código viejo, mapeando IDs viejos a notas MIDI
    void playSound(int id, float xPosition, float worldWidth) {
        // Mapeo trucho: Si piden ID 1 (Do), tocamos MIDI 60 (Do central)
        // Esto es solo para que no crashee si usas el modo viejo.
        int midiMap[] = { 0, 60, 62, 64, 65, 67, 69, 71, 72 };
        if (id > 0 && id <= 8) playMidiNote(midiMap[id]);
    }

private:
    void sendToRecorder(const std::int16_t* samples, std::size_t count, float vol);

    // Cambiamos el nombre para ser claros
    std::map<int, sf::SoundBuffer> midiBuffers;
    std::vector<std::optional<sf::Sound>> soundPool;
    Recorder* recorder = nullptr;
    std::mt19937 rng;
};