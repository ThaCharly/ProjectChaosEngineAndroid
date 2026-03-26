#pragma once

#include <string>
#include <cstdio>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <SFML/Graphics.hpp>
#include <SFML/OpenGL.hpp> // <--- Magia de OpenGL
#include <SFML/Audio.hpp> 

class Recorder {
public:
    Recorder(int width, int height, int fps, const std::string& outputFilename);
    ~Recorder();

    void addFrame(const sf::Texture& texture);
    void addAudioEvent(const sf::Int16* samples, std::size_t sampleCount, float volume);
    void stop(); 

    bool isRecording = false; 

private:
    void workerLoop(); 

    FILE* ffmpegPipe = nullptr;
    int width;
    int height;
    int fps;
    std::string finalFilename;      
    std::string tempVideoFilename;  
    std::string tempAudioFilename;  

    std::vector<float> audioMixBuffer; 
    unsigned int sampleRate = 44100;
    long long currentFrame = 0; 
    
    bool isFinished = false; 

    // --- MULTITHREADING ---
    std::thread workerThread;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::condition_variable queueSpaceCV;
    std::queue<std::vector<sf::Uint8>> frameQueue;
    std::atomic<bool> isWorkerRunning;

    const size_t MAX_QUEUE_SIZE = 480; // 60 son aproximadamente 1.1GB de RAM, 480 son 8.8GB

    // --- PBOs (Pixel Buffer Objects) ---
    GLuint pbo[2];
    int pboIndex = 0;
    int nextPboIndex = 1;
    bool firstFrame = true;
};