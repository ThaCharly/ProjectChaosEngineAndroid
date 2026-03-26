#pragma once

#include <SFML/Audio.hpp>
#include <vector>
#include <map>
#include <cmath>
#include <iostream>
#include <random>

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
        for(int i=0; i<64; ++i) soundPool.emplace_back();
    }

    void setRecorder(Recorder* rec) {
        recorder = rec;
    }

    // Generador de ondas (Senoide suave)
    void generateTone(int id, float frequency) {
        const unsigned SAMPLE_RATE = 44100;
        const int AMPLITUDE = 18000; 

        std::vector<sf::Int16> rawSamples;
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

            rawSamples.push_back((sf::Int16)(wave * envelope * AMPLITUDE));
        }

        sf::SoundBuffer buffer;
        if (buffer.loadFromSamples(&rawSamples[0], rawSamples.size(), 1, SAMPLE_RATE)) {
            midiBuffers[id] = buffer;
        }
    }

    // ESTA ES LA NUEVA FUNCIÓN CLAVE
    void playMidiNote(int noteNumber, float volume = 98.0f) {
        if (noteNumber < 0 || noteNumber > 127) return;
        if (midiBuffers.find(noteNumber) == midiBuffers.end()) return;

        sf::Sound* sound = getFreeSound();
        if (sound) {
            sound->setBuffer(midiBuffers[noteNumber]);
            sound->setVolume(volume); 
            sound->setPitch(1.0f);
            sound->setPosition(0, 0, 0); // Sonido 2D plano para la música
            sound->setAttenuation(0);    // Que se escuche igual en todos lados
            sound->play();
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
    sf::Sound* getFreeSound() {
        for (auto& s : soundPool) {
            if (s.getStatus() == sf::Sound::Stopped) return &s;
        }
        return &soundPool[0]; 
    }

    void sendToRecorder(const sf::Int16* samples, std::size_t count, float vol);

    // Cambiamos el nombre para ser claros
    std::map<int, sf::SoundBuffer> midiBuffers;
    std::vector<sf::Sound> soundPool;
    Recorder* recorder = nullptr;
    std::mt19937 rng;
};