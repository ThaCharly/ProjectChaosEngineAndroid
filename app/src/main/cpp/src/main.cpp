#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <box2d/box2d.h>
#include <imgui.h>
#include <imgui-SFML.h>
#include <iostream>
#ifndef __ANDROID__
#include <filesystem>
#endif
#include <string>
#include <deque>
#include <cstdlib>
#include <memory>
#include <cstdint>

#include "Physics/PhysicsWorld.hpp"
#include "Recorder/Recorder.hpp"
#include "Sound/SoundManager.hpp"
#include "Utils/InputManager.hpp"

#ifndef __ANDROID__
namespace fs = std::filesystem;
#endif

struct TrailPoint {
    sf::Vector2f pos;
    float time;
};

struct Trail {
    std::deque<TrailPoint> points;
    sf::Color color;
    float currentDuration = 0.1f;
};

struct AmbientParticle {
    sf::Vector2f basePos;
    float yPos;
    float speedY;
    float phaseOffset;
    float phaseSpeed;
    float amplitude;
    float size;
    sf::Color color;
};

const char* brightnessFrag = R"(
    #if defined(GL_ES)
    precision mediump float;
    #endif
    uniform sampler2D source;
    uniform float threshold;
    void main() {
        vec4 color = texture2D(source, gl_TexCoord[0].xy);

        // NORMALIZACIÓN DE NEÓN:
        // Usamos el canal más alto del pixel en lugar de la luminancia del ojo humano.
        // Así un Azul puro (0,0,1) y un Verde puro (0,1,0) tienen un brillo = 1.0.
        float maxBrightness = max(color.r, max(color.g, color.b));

        if (maxBrightness > threshold) {
            gl_FragColor = color;
        } else {
            gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        }
    }
)";

// Desenfoque Gaussiano Separable (Muchísimo más rápido que el bidimensional)
const char* blurFrag = R"(
    #if defined(GL_ES)
    precision mediump float;
    #endif
    uniform sampler2D source;
    uniform vec2 dir; // Dirección del desenfoque (horizontal o vertical)
    void main() {
        vec2 uv = gl_TexCoord[0].xy;
        vec4 color = vec4(0.0);
        vec2 off1 = vec2(1.3846153846) * dir;
        vec2 off2 = vec2(3.2307692308) * dir;

        color += texture2D(source, uv) * 0.2270270270;
        color += texture2D(source, uv + off1) * 0.3162162162;
        color += texture2D(source, uv - off1) * 0.3162162162;
        color += texture2D(source, uv + off2) * 0.0702702703;
        color += texture2D(source, uv - off2) * 0.0702702703;

        gl_FragColor = color;
    }
)";

const char* blendFrag = R"(
    #if defined(GL_ES)
    precision mediump float;
    #endif
    uniform sampler2D baseTexture;
    uniform sampler2D bloomTexture;
    uniform float multiplier;
    void main() {
        vec4 base = texture2D(baseTexture, gl_TexCoord[0].xy);
        vec4 bloom = texture2D(bloomTexture, gl_TexCoord[0].xy);
        // Fusión Aditiva: Luz + Luz
        gl_FragColor = base + (bloom * multiplier);
    }
)";

// 1. Arriba de todo, cambiá la grilla para que responda a la resolución
sf::Texture createGridTexture(int width, int height) {
    sf::RenderTexture rt;
    rt.resize({(unsigned int)width, (unsigned int)height});
    rt.clear(sf::Color::Transparent); // <--- MAGIA ACÁ: Fondo transparente
    sf::RectangleShape line;
    line.setFillColor(sf::Color(10, 10, 10));

    float lineThick = (width / 1080.0f) * 2.0f;
    int stepSize = width / 18;

    line.setSize({lineThick, (float)height});
    for (int x = 0; x < width; x += stepSize) {
        line.setPosition({(float)x, 0.0f}); rt.draw(line);
    }
    line.setSize({(float)width, lineThick});
    for (int y = 0; y < height; y += stepSize) {
        line.setPosition({0.0f, (float)y}); rt.draw(line);
    }
    rt.display();
    return rt.getTexture();
}

sf::Color lerpColor(const sf::Color& a, const sf::Color& b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return sf::Color(
            (std::uint8_t)(a.r + (b.r - a.r) * t),
            (std::uint8_t)(a.g + (b.g - a.g) * t),
            (std::uint8_t)(a.b + (b.b - a.b) * t),
            (std::uint8_t)(a.a + (b.a - a.a) * t)
    );
}

void SoundManager::sendToRecorder(const std::int16_t* samples, std::size_t count, float vol) {
    if (recorder) {
        recorder->addAudioEvent(samples, count, vol);
    }
}

// --- MÁQUINA DE ESTADOS PARA LA UI ESTILO UNITY ---
enum class EntityType { None, Global, WinZone, Racers, Wall, Knife };

int main()
{
#ifdef __ANDROID__
    // Bajamos a 1080p interno en mobile para que el Bloom no te prenda fuego el celular
    const unsigned int RENDER_WIDTH = 1080;
    const unsigned int RENDER_HEIGHT = 1080;
#else
    const unsigned int RENDER_WIDTH = 2160;
    const unsigned int RENDER_HEIGHT = 2160;
#endif
 //   const unsigned int FPS = 60;
    const std::string VIDEO_DIRECTORY = "../output/video.mp4";

    int simFPS = 60;
    int recordFPS = 60;

    sf::VideoMode desktopMode = sf::VideoMode::getDesktopMode();
    sf::ContextSettings settings;
    settings.majorVersion = 2;
    settings.minorVersion = 0;
    sf::RenderWindow window(sf::VideoMode(desktopMode), "ChaosEngine", sf::Style::Default, sf::State::Fullscreen, settings);
    window.setFramerateLimit(simFPS);

    if (!ImGui::SFML::Init(window)) return -1;

    // --- ESCALADO PARA MÓVILES (Fat Finger UX) ---
    bool isMobile = (desktopMode.size.x < 800);
    if (isMobile) {
        ImGui::GetStyle().ScaleAllSizes(2.0f);
        ImGui::GetIO().FontGlobalScale = 2.0f;
    }

    // --- ESTILO IMGUI TIPO MOTOR GRÁFICO ---
// --- ESTILO IMGUI TIPO MOTOR GRÁFICO (REFINADO UE5/UNITY) ---
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;

    // MAGIA UX: Márgenes dinámicos. Si es mobile, todo respira más para los dedos.
    style.ItemSpacing = ImVec2(isMobile ? 12.0f : 8.0f, isMobile ? 10.0f : 6.0f);
    style.FramePadding = ImVec2(isMobile ? 12.0f : 8.0f, isMobile ? 10.0f : 4.0f);
    style.WindowPadding = ImVec2(12.0f, 12.0f);

    // Paleta Dark Mode moderna (Gris azulado profundo)
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.11f, 0.98f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.27f, 0.50f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.17f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.27f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.22f, 0.22f, 0.24f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.35f, 0.35f, 0.37f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.33f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.40f, 0.44f, 1.0f);

    // Acentos visuales para feedback claro
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.0f, 0.7f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.0f, 0.7f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.0f, 0.8f, 1.0f, 1.0f);

    sf::RenderTexture gameBuffer;
    if (!gameBuffer.resize({RENDER_WIDTH, RENDER_HEIGHT})) {
        std::cerr << "Pah, te quedaste sin VRAM bo. Falló el RenderTexture." << std::endl;
        return -1;
    }

    // --- SETUP DE BLOOM ---
#ifndef __ANDROID__
    if (!sf::Shader::isAvailable()) {
        std::cerr << "Pah, tu GPU no banca shaders. Olvidate del neón." << std::endl;
        return -1;
    }
#endif

    sf::Font uiFont;
#ifdef __ANDROID__
    // En Android, la raíz es directamente la carpeta que Gradle empaqueta como 'assets'
    uiFont.openFromFile("fonts/jetbrains_mono.ttf");
#else
    uiFont.openFromFile("../fonts/jetbrains_mono.ttf");
#endif

        sf::Texture knifeTex;
        // Si tenés una carpeta assets, meté el PNG ahí con el nombre "knife.png"
    #ifdef __ANDROID__
        bool hasKnifeTex = knifeTex.loadFromFile("knife.png"); // El AssetManager entra directo acá
    #else
        bool hasKnifeTex = knifeTex.loadFromFile("../assets/knife.png");
    #endif
    if (hasKnifeTex) {
        knifeTex.setSmooth(true);
        std::cout << ">>> Asset de cuchillo (499x499) cargado joya." << std::endl;
    } else {
        std::cout << ">>> No se encontro knife.png, usando hoja por defecto." << std::endl;
    }

    sf::Shader brightnessShader, blurShader, blendShader;
#ifndef __ANDROID__
    brightnessShader.loadFromMemory(brightnessFrag, sf::Shader::Type::Fragment);
    blurShader.loadFromMemory(blurFrag, sf::Shader::Type::Fragment);
    blendShader.loadFromMemory(blendFrag, sf::Shader::Type::Fragment);
#endif

    // Achicamos a la mitad para el cálculo del brillo. ¡Magia negra para optimizar!
    unsigned int BLOOM_W = RENDER_WIDTH / 2;
    unsigned int BLOOM_H = RENDER_HEIGHT / 2;

    sf::RenderTexture brightnessBuffer, blurBuffer1, blurBuffer2, finalBuffer;
    brightnessBuffer.resize({BLOOM_W, BLOOM_H});
    blurBuffer1.resize({BLOOM_W, BLOOM_H});
    blurBuffer2.resize({BLOOM_W, BLOOM_H});
    finalBuffer.resize({RENDER_WIDTH, RENDER_HEIGHT}); // Este es el 4K final que grabamos

    // Variables de control para ImGui
#ifdef __ANDROID__
    bool enableBloom = false;
#else
    bool enableBloom = true;
#endif
    float bloomThreshold = 0.9f; // A partir de qué brillo empieza a generar glow
    float bloomMultiplier = 0.5f; // Intensidad del neón
    int blurIterations = 3; // Cuántas pasadas de blur (más = glow más grande)

    std::unique_ptr<Recorder> recorder = nullptr;
    SoundManager soundManager;
    PhysicsWorld physics(RENDER_WIDTH, RENDER_HEIGHT, &soundManager);
    physics.isPaused = true;
    const auto& bodies = physics.getDynamicBodies();

    #ifndef __ANDROID__
    fs::path videoPath(VIDEO_DIRECTORY);
    fs::path outputDir = videoPath.parent_path();
    if (!fs::exists(outputDir)) fs::create_directories(outputDir);
#endif

    const float timeStep = 1.0f / 60.0f;
    int32 velIter = 8;
    int32 posIter = 3;

    sf::Clock clock;
    sf::Clock deltaClock;
    float accumulator = 0.0f;
    float globalTime = 0.0f;

    float victoryTimer = 0.0f;
    bool victorySequenceStarted = false;
    const float VICTORY_DELAY = 0.5f;

    const char* racerNames[] = { "Cyan", "Magenta", "Green", "Yellow" };
    sf::Color racerColors[] = {
        sf::Color(0, 255, 255),
        sf::Color(255, 0, 255),
        sf::Color(57, 255, 20),
        sf::Color(255, 215, 0)
    };
    ImVec4 guiColors[] = {
        ImVec4(0, 1, 1, 1),
        ImVec4(1, 0, 1, 1),
        ImVec4(0.2f, 1, 0.1f, 1),
        ImVec4(1, 0.8f, 0, 1)
    };

    sf::Texture gridTexture = createGridTexture(RENDER_WIDTH, RENDER_HEIGHT);
    sf::Sprite background(gridTexture);

    std::vector<Trail> trails(4);
    for(int i=0; i<4; ++i) trails[i].color = racerColors[i];

    #ifdef __ANDROID__
    static char mapFilename[128] = "levels/level_01.txt";
#else
    static char mapFilename[128] = "../levels/level_01.txt";
#endif
    static char songFile[128] = "song.txt";

    // Variables de estado de la Interfaz
    EntityType selectedType = EntityType::None;
    int selectedIndex = -1;

    // --- SETUP DE POLVO ATMOSFÉRICO ---
    // --- SETUP DE POLVO ATMOSFÉRICO REFINADO ---
    std::vector<AmbientParticle> ambientDust;
    const int NUM_DUST = 70; // Bajamos la cantidad
    for (int i = 0; i < NUM_DUST; ++i) {
        AmbientParticle p;
        p.basePos.x = (float)(std::rand() % RENDER_WIDTH);
        p.yPos = (float)(std::rand() % RENDER_HEIGHT);

        // Más velocidad vertical: de 20 a 60 px/s (antes era 5-30)
        p.speedY = -((float)(std::rand() % 40) + 20.0f);

        p.phaseOffset = (float)(std::rand() % 628) / 100.0f;

        // Vaivén más rápido: frecuencia de oscilación aumentada
        p.phaseSpeed = ((float)(std::rand() % 25) + 15.0f) / 10.0f;

        // Amplitud mucho mayor: recorren más espacio horizontal (30 a 110 px)
        p.amplitude = (float)(std::rand() % 80) + 30.0f;

        p.size = (float)(std::rand() % 3) + 2.0f;

        // Mantenemos un alpha bajísimo para que sea un detalle sutil
        std::uint8_t alpha = 15 + (std::rand() % 25);
        p.color = sf::Color(180, 230, 255, alpha);
        ambientDust.push_back(p);
    }

// Variables para los Gizmos y Drag & Drop
    enum class GizmoState { None, Moving, Rotating, Scaling };
    GizmoState currentGizmo = GizmoState::None;
    b2Vec2 dragOffset(0.0f, 0.0f);

    // Guardamos el estado inicial exacto para usar "Deltas" (cero saltos)
    float initialMouseAngle = 0.0f;
    float initialRotation = 0.0f;
    float initialWidth = 0.0f;
    float initialHeight = 0.0f;
    b2Vec2 initialPos(0.0f, 0.0f);
    b2Vec2 initialMouseLocal(0.0f, 0.0f);

    // Variables de UX
    int activeScaleCorner = -1; // 0=TL, 1=TR, 2=BR, 3=BL
    int hoveredScaleCorner = -1;
    bool isHoveringRotate = false;
    bool isHoveringMove = false;

    InputManager inputManager; // <-- ACÁ

    bool appActive = true;

    while (window.isOpen()) {

        while (const std::optional<sf::Event> event = window.pollEvent()) {
            ImGui::SFML::ProcessEvent(window, *event);

            if (event->is<sf::Event::Closed>()) window.close();

            if (const auto* key = event->getIf<sf::Event::KeyPressed>()) {
                if (key->scancode == sf::Keyboard::Scancode::Escape) window.close();
            }

            // --- MANEJO DEL CICLO DE VIDA EN ANDROID ---
            if (event->is<sf::Event::FocusLost>()) appActive = false;
            if (event->is<sf::Event::FocusGained>()) appActive = true;

            if (const auto* resized = event->getIf<sf::Event::Resized>()) {
                sf::FloatRect visibleArea({0.f, 0.f}, sf::Vector2f(resized->size));
                window.setView(sf::View(visibleArea));
            }
        }

        // Si la app está minimizada, dormimos el hilo para no gastar batería ni crashear el EGL Surface
        if (!appActive) {
            sf::sleep(sf::milliseconds(100));
            continue;
        }

        ImGui::SFML::Update(window, deltaClock.restart());

// ==========================================
        // SISTEMA DE PICKING Y GIZMOS (MOVE, ROTATE, SCALE x4)
        // ==========================================
        // ==========================================
        // SISTEMA DE PICKING Y GIZMOS (MOVE, ROTATE, SCALE x4)
        // ==========================================
ImGuiIO& io = ImGui::GetIO();

        // 1. Leemos la verdad absoluta desde SFML, no desde ImGui
        sf::Vector2u winSize = window.getSize();

        // EL FIX DEFINITIVO: Obligamos a la cámara de SFML a calzar 1:1 con la ventana física CADA FRAME.
        // Esto mata cualquier estiramiento o recorte que haga el SO por atrás.
        window.setView(sf::View(sf::FloatRect({0.0f, 0.0f}, {(float)winSize.x, (float)winSize.y})));

        float screenWidth = (float)winSize.x;
        float screenHeight = (float)winSize.y;
        isMobile = (screenWidth < 800.0f); // Refresco dinámico

        float leftPanelWidth = isMobile ? screenWidth : screenWidth * 0.15f;
        float rightPanelWidth = isMobile ? screenWidth : screenWidth * 0.20f;
        float toolbarHeight = isMobile ? 80.0f : 65.0f;

        // MAGIA ANTI-CORTE: 20 píxeles de margen (padding) para que el SO no te coma la pared 0
        float safePadding = 20.0f;

        float availableWidth = (isMobile ? screenWidth : (screenWidth - leftPanelWidth - rightPanelWidth)) - (safePadding * 2.0f);
        float availableHeight = screenHeight - toolbarHeight - (safePadding * 2.0f);

        // Escalar manteniendo el Aspect Ratio exacto
        float scaleBase = std::min(availableWidth / RENDER_WIDTH, availableHeight / RENDER_HEIGHT);

        // Calculamos el offset final empujándolo con el safePadding
        float offsetX = (isMobile ? 0.0f : leftPanelWidth) + safePadding + (availableWidth - (RENDER_WIDTH * scaleBase)) / 2.0f;
        float offsetY = toolbarHeight + safePadding + (availableHeight - (RENDER_HEIGHT * scaleBase)) / 2.0f;

        // Reseteamos el estado visual frame a frame
        hoveredScaleCorner = -1;
        isHoveringRotate = false;
        isHoveringMove = false;

        inputManager.update(window, io.WantCaptureMouse);
        const auto& ptr = inputManager.getPointer();

        if (!ptr.handledByUI) {
            float bufferX = (ptr.position.x - offsetX) / scaleBase;
            float bufferY = (ptr.position.y - offsetY) / scaleBase;
            float box2dX = bufferX / physics.SCALE;
            float box2dY = bufferY / physics.SCALE;
            b2Vec2 mouseB2(box2dX, box2dY);

            // Tolerancias dinámicas para dedos (móvil) o mouse (PC)
            float gizmoTolerance = isMobile ? 3.0f : 1.2f;
            float movePadding = isMobile ? 2.0f : 0.8f;

            auto toGlobal = [&](b2Vec2 local, b2Vec2 objPos, float angle) -> b2Vec2 {
                float c = std::cos(angle); float s = std::sin(angle);
                return b2Vec2(objPos.x + local.x * c - local.y * s, objPos.y + local.x * s + local.y * c);
            };
            auto toLocal = [&](b2Vec2 global, b2Vec2 objPos, float angle) -> b2Vec2 {
                float c = std::cos(angle); float s = std::sin(angle);
                float dx = global.x - objPos.x; float dy = global.y - objPos.y;
                return b2Vec2(dx * c + dy * s, -dx * s + dy * c);
            };

            // MAGIA NUEVA: Colisión con margen de error (Padding) para que los hovers sean suaves
            auto pointInWall = [&](b2Vec2 p, const CustomWall& w, float padding) {
                b2Vec2 localP = toLocal(p, w.body->GetPosition(), w.body->GetAngle());
                return std::abs(localP.x) <= (w.width / 2.0f + padding) &&
                       std::abs(localP.y) <= (w.height / 2.0f + padding);
            };

            // --- DETECCIÓN DE HOVER (Cursores y Gizmos) ---
            if (currentGizmo == GizmoState::None) {
                if (selectedType == EntityType::Wall && selectedIndex >= 0 && selectedIndex < physics.getCustomWalls().size()) {
                    CustomWall& w = physics.getCustomWalls()[selectedIndex];
                    b2Vec2 rotHandleGlobal = w.body->GetWorldPoint(b2Vec2(0.0f, -w.height / 2.0f - 1.5f));
                    b2Vec2 corners[4] = {
                        w.body->GetWorldPoint(b2Vec2(-w.width/2.0f, -w.height/2.0f)), // TL
                        w.body->GetWorldPoint(b2Vec2( w.width/2.0f, -w.height/2.0f)), // TR
                        w.body->GetWorldPoint(b2Vec2( w.width/2.0f,  w.height/2.0f)), // BR
                        w.body->GetWorldPoint(b2Vec2(-w.width/2.0f,  w.height/2.0f))  // BL
                    };

                    if ((mouseB2 - rotHandleGlobal).Length() < gizmoTolerance) {
                        isHoveringRotate = true;
                    } else {
                        float minDist = gizmoTolerance;
                        for (int c = 0; c < 4; c++) {
                            float dist = (mouseB2 - corners[c]).Length();
                            if (dist < minDist) {
                                hoveredScaleCorner = c;
                                minDist = dist;
                            }
                        }
                    }
                }
                else if (selectedType == EntityType::WinZone) {
                    float wzW = physics.winZoneSize[0];
                    float wzH = physics.winZoneSize[1];
                    b2Vec2 wzPos(physics.winZonePos[0], physics.winZonePos[1]);
                    b2Vec2 corners[4] = {
                        wzPos + b2Vec2(-wzW/2.0f, -wzH/2.0f),
                        wzPos + b2Vec2( wzW/2.0f, -wzH/2.0f),
                        wzPos + b2Vec2( wzW/2.0f,  wzH/2.0f),
                        wzPos + b2Vec2(-wzW/2.0f,  wzH/2.0f)
                    };

                    float minDist = gizmoTolerance;
                    for (int c = 0; c < 4; c++) {
                        float dist = (mouseB2 - corners[c]).Length();
                        if (dist < minDist) {
                            hoveredScaleCorner = c;
                            minDist = dist;
                        }
                    }
                }

                // Fallback para cursor de Mover (Usamos PADDING para que no titile)
                if (!isHoveringRotate && hoveredScaleCorner == -1) {
                    // Priorizamos chequear el objeto que YA está seleccionado para que no lo suelte fácil
                    if (selectedType == EntityType::Racers && selectedIndex >= 0 && selectedIndex < bodies.size()) {
                        if ((mouseB2 - bodies[selectedIndex]->GetPosition()).Length() < (physics.currentRacerSize / 2.0f) + movePadding) isHoveringMove = true;
                    } else if (selectedType == EntityType::Wall && selectedIndex >= 0 && selectedIndex < physics.getCustomWalls().size()) {
                        if (pointInWall(mouseB2, physics.getCustomWalls()[selectedIndex], movePadding)) isHoveringMove = true;
                    } else if (selectedType == EntityType::WinZone) {
                        if (std::abs(mouseB2.x - physics.winZonePos[0]) <= physics.winZoneSize[0]/2.0f + movePadding &&
                            std::abs(mouseB2.y - physics.winZonePos[1]) <= physics.winZoneSize[1]/2.0f + movePadding) isHoveringMove = true;
                    }

                    // Si no está arriba del seleccionado, chequeamos el resto de forma más ajustada
                    if (!isHoveringMove) {
                        for (int i = 0; i < bodies.size(); ++i) {
                            if ((mouseB2 - bodies[i]->GetPosition()).Length() < physics.currentRacerSize / 2.0f + movePadding) { isHoveringMove = true; break; }
                        }
                        if (!isHoveringMove) {
                            for (int i = 0; i < physics.getCustomWalls().size(); ++i) {
                                if (pointInWall(mouseB2, physics.getCustomWalls()[i], 0.0f)) { isHoveringMove = true; break; }
                            }
                        }
                        if (!isHoveringMove) {
                            if (std::abs(mouseB2.x - physics.winZonePos[0]) <= physics.winZoneSize[0]/2.0f && std::abs(mouseB2.y - physics.winZonePos[1]) <= physics.winZoneSize[1]/2.0f) isHoveringMove = true;
                        }
                    }
                }
            }

            // --- APLICACIÓN DE CURSORES DINÁMICOS ---
            int cursorCorner = (currentGizmo == GizmoState::Scaling) ? activeScaleCorner : hoveredScaleCorner;
            if (cursorCorner == 0 || cursorCorner == 2) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            else if (cursorCorner == 1 || cursorCorner == 3) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
            else if (currentGizmo == GizmoState::Rotating || isHoveringRotate) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            else if (currentGizmo == GizmoState::Moving || isHoveringMove) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

            // --- LÓGICA DE CLICS Y ARRASTRE ---
            if (ptr.state == PointerState::Pressed) {
                if (currentGizmo == GizmoState::None) {
                    bool handleClicked = false;

                    // 1. Chequeamos si tocamos un Gizmo activo del objeto seleccionado
                    if (selectedType == EntityType::Wall && selectedIndex >= 0) {
                        CustomWall& w = physics.getCustomWalls()[selectedIndex];
                        if (isHoveringRotate) {
                            currentGizmo = GizmoState::Rotating;
                            initialMouseAngle = std::atan2(mouseB2.y - w.body->GetPosition().y, mouseB2.x - w.body->GetPosition().x);
                            initialRotation = w.body->GetAngle();
                            handleClicked = true;
                        } else if (hoveredScaleCorner != -1) {
                            currentGizmo = GizmoState::Scaling;
                            activeScaleCorner = hoveredScaleCorner;
                            initialPos = w.body->GetPosition();
                            initialWidth = w.width;
                            initialHeight = w.height;
                            initialMouseLocal = toLocal(mouseB2, initialPos, w.body->GetAngle());
                            handleClicked = true;
                        }
                    } else if (selectedType == EntityType::WinZone && hoveredScaleCorner != -1) {
                        currentGizmo = GizmoState::Scaling;
                        activeScaleCorner = hoveredScaleCorner;
                        initialPos = b2Vec2(physics.winZonePos[0], physics.winZonePos[1]);
                        initialWidth = physics.winZoneSize[0];
                        initialHeight = physics.winZoneSize[1];
                        initialMouseLocal = mouseB2 - initialPos; // AABB no rota
                        handleClicked = true;
                    }

                    // 2. Si no tocamos gizmos, seleccionamos cuerpos
                    if (!handleClicked) {
                        int clickedRacer = -1;
                        for (int i = 0; i < bodies.size(); ++i) {
                            if ((mouseB2 - bodies[i]->GetPosition()).Length() < (physics.currentRacerSize / 2.0f) + movePadding) { clickedRacer = i; break; }
                        }

                        if (clickedRacer != -1) {
                            selectedType = EntityType::Racers;
                            selectedIndex = clickedRacer;
                            currentGizmo = GizmoState::Moving;
                            dragOffset = mouseB2 - bodies[clickedRacer]->GetPosition();
                        } else {
                            int clickedWall = -1;
                            // Prioridad a la pared seleccionada por si la estás agarrando del borde
                            if (selectedType == EntityType::Wall && selectedIndex >= 0 && selectedIndex < physics.getCustomWalls().size()) {
                                if (pointInWall(mouseB2, physics.getCustomWalls()[selectedIndex], movePadding)) clickedWall = selectedIndex;
                            }
                            // Si no, buscamos cualquier otra
                            if (clickedWall == -1) {
                                for (int i = (int)physics.getCustomWalls().size() - 1; i >= 0; --i) {
                                    if (pointInWall(mouseB2, physics.getCustomWalls()[i], 0.0f)) { clickedWall = i; break; }
                                }
                            }

                            if (clickedWall != -1) {
                                selectedType = EntityType::Wall;
                                selectedIndex = clickedWall;
                                currentGizmo = GizmoState::Moving;
                                b2Vec2 objPos = physics.getCustomWalls()[clickedWall].body->GetPosition();
                                dragOffset = b2Vec2(box2dX - objPos.x, box2dY - objPos.y);
                            } else if (std::abs(mouseB2.x - physics.winZonePos[0]) <= physics.winZoneSize[0]/2.0f + movePadding &&
                                       std::abs(mouseB2.y - physics.winZonePos[1]) <= physics.winZoneSize[1]/2.0f + movePadding) {
                                selectedType = EntityType::WinZone;
                                currentGizmo = GizmoState::Moving;
                                dragOffset = mouseB2 - b2Vec2(physics.winZonePos[0], physics.winZonePos[1]);
                            } else {
                                selectedType = EntityType::None;
                                selectedIndex = -1;
                            }
                        }
                    }
                }
            } else if (ptr.state == PointerState::Held) {
                // --- FASE DE ARRASTRE ---
                if (currentGizmo == GizmoState::Moving) {
                    if (selectedType == EntityType::Wall && selectedIndex >= 0) {
                        CustomWall& w = physics.getCustomWalls()[selectedIndex];
                        w.body->SetTransform(b2Vec2(box2dX - dragOffset.x, box2dY - dragOffset.y), w.rotation);
                        w.body->SetAwake(true);
                    }
                    else if (selectedType == EntityType::Racers && selectedIndex >= 0) {
                        b2Body* b = bodies[selectedIndex];
                        b->SetTransform(b2Vec2(box2dX - dragOffset.x, box2dY - dragOffset.y), b->GetAngle());
                        b->SetLinearVelocity(b2Vec2(0.0f, 0.0f));
                        b->SetAwake(true);
                    }
                    else if (selectedType == EntityType::WinZone) {
                        physics.winZonePos[0] = box2dX - dragOffset.x;
                        physics.winZonePos[1] = box2dY - dragOffset.y;
                        physics.updateWinZone(physics.winZonePos[0], physics.winZonePos[1], physics.winZoneSize[0], physics.winZoneSize[1]);
                    }
                }
                else if (currentGizmo == GizmoState::Rotating && selectedType == EntityType::Wall && selectedIndex >= 0) {
                    CustomWall& w = physics.getCustomWalls()[selectedIndex];
                    float currentMouseAngle = std::atan2(mouseB2.y - w.body->GetPosition().y, mouseB2.x - w.body->GetPosition().x);
                    float newAngle = initialRotation + (currentMouseAngle - initialMouseAngle);
                    w.rotation = newAngle;
                    w.body->SetTransform(w.body->GetPosition(), newAngle);
                    w.body->SetAwake(true);
                }
                else if (currentGizmo == GizmoState::Scaling) {
                    float deltaX, deltaY;
                    float sx = (activeScaleCorner == 1 || activeScaleCorner == 2) ? 1.0f : -1.0f;
                    float sy = (activeScaleCorner == 2 || activeScaleCorner == 3) ? 1.0f : -1.0f;

                    if (selectedType == EntityType::Wall && selectedIndex >= 0) {
                        CustomWall& w = physics.getCustomWalls()[selectedIndex];
                        b2Vec2 currentMouseLocal = toLocal(mouseB2, initialPos, w.rotation);
                        deltaX = currentMouseLocal.x - initialMouseLocal.x;
                        deltaY = currentMouseLocal.y - initialMouseLocal.y;

                        float newW = std::max(0.5f, initialWidth + (deltaX * sx));
                        float newH = std::max(0.5f, initialHeight + (deltaY * sy));

                        b2Vec2 fixedLocal(-sx * initialWidth / 2.0f, -sy * initialHeight / 2.0f);
                        b2Vec2 newCenterLocal((fixedLocal.x + (fixedLocal.x + sx * newW)) / 2.0f, (fixedLocal.y + (fixedLocal.y + sy * newH)) / 2.0f);
                        b2Vec2 newCenterGlobal = toGlobal(newCenterLocal, initialPos, w.rotation);

                        physics.updateCustomWall(selectedIndex, newCenterGlobal.x, newCenterGlobal.y, newW, newH, w.soundID, w.shapeType, w.rotation);
                        w.body->SetAwake(true);
                    }
                    else if (selectedType == EntityType::WinZone) {
                        b2Vec2 currentMouseLocal = mouseB2 - initialPos;
                        deltaX = currentMouseLocal.x - initialMouseLocal.x;
                        deltaY = currentMouseLocal.y - initialMouseLocal.y;

                        float newW = std::max(0.5f, initialWidth + (deltaX * sx));
                        float newH = std::max(0.5f, initialHeight + (deltaY * sy));

                        b2Vec2 fixedLocal(-sx * initialWidth / 2.0f, -sy * initialHeight / 2.0f);
                        b2Vec2 newCenterLocal((fixedLocal.x + (fixedLocal.x + sx * newW)) / 2.0f, (fixedLocal.y + (fixedLocal.y + sy * newH)) / 2.0f);
                        b2Vec2 newCenterGlobal = initialPos + newCenterLocal;

                        physics.winZonePos[0] = newCenterGlobal.x;
                        physics.winZonePos[1] = newCenterGlobal.y;
                        physics.winZoneSize[0] = newW;
                        physics.winZoneSize[1] = newH;
                        physics.updateWinZone(physics.winZonePos[0], physics.winZonePos[1], physics.winZoneSize[0], physics.winZoneSize[1]);
                    }
                }
            } else if (ptr.state == PointerState::Released) {
                currentGizmo = GizmoState::None;
            }
        } else {
            currentGizmo = GizmoState::None;
        }

sf::Time dt = clock.restart();
        float dtSec = dt.asSeconds();

        float timeStep = 1.0f / (float)simFPS; // Paso dinámico para tiempo real

        if (recorder && recorder->isRecording) {
            // Override absoluto: Clavamos el tiempo a la frecuencia de grabación
            dtSec = 1.0f / (float)recordFPS;
            timeStep = dtSec;
        }

        physics.updateWallVisuals(dtSec);
        physics.updateParticles(dtSec);
        globalTime += dtSec;

        if (!physics.isPaused) {
            if (recorder && recorder->isRecording) {
                // Grabando: Avanza la física fotograma a fotograma como reloj suizo
                physics.step(timeStep, velIter, posIter);
                physics.updateWallExpansion(timeStep);
                physics.updateMovingPlatforms(timeStep);
            } else {
                // Tiempo Real: El acumulador te salva de cualquier lagazo
                accumulator += dtSec;
                while (accumulator >= timeStep) {
                    physics.step(timeStep, velIter, posIter);
                    physics.updateWallExpansion(timeStep);
                    physics.updateMovingPlatforms(timeStep);
                    accumulator -= timeStep;
                }
            }
        } else {
            accumulator = 0.0f;
        }

if (!physics.isPaused) {
            for (size_t i = 0; i < bodies.size(); ++i) {
                if (i >= trails.size()) break;
                b2Vec2 pos = bodies[i]->GetPosition();
                sf::Vector2f p(pos.x * physics.SCALE, pos.y * physics.SCALE);

                float speed = bodies[i]->GetLinearVelocity().Length();
                float targetDuration = (((speed * 1.5f) + 5.0f) / 60.0f) * 0.8f;
                trails[i].currentDuration = targetDuration; // Guardamos para el render

                // SUBDIVISIÓN GEOMÉTRICA: Si hay poca densidad (bajos FPS) y mucha distancia,
                // metemos vértices extra para que nunca se vea "tosca o cuadrada".
                if (!trails[i].points.empty()) {
                    sf::Vector2f lastP = trails[i].points.front().pos;
                    float lastTime = trails[i].points.front().time;

                    float dx = p.x - lastP.x;
                    float dy = p.y - lastP.y;
                    float dist = std::sqrt(dx*dx + dy*dy);

                    int segments = (int)(dist / 6.0f) + 1; // Un vértice cada 6 píxeles máximo
                    if (segments > 1 && segments < 20) {
                        for (int k = 1; k < segments; ++k) {
                            float t = (float)k / (float)segments;
                            sf::Vector2f interpP = lastP + sf::Vector2f(dx * t, dy * t);
                            float interpTime = lastTime + (globalTime - lastTime) * t;
                            trails[i].points.push_front({interpP, interpTime});
                        }
                    }
                }

                // Agregamos el punto original del frame
                trails[i].points.push_front({p, globalTime});

                // Limpieza por tiempo real
                while (!trails[i].points.empty() && (globalTime - trails[i].points.back().time) > targetDuration) {
                    trails[i].points.pop_back();
                }
            }
}

        if (physics.gameOver) {
            if (!victorySequenceStarted) {
                std::cout << ">>> VICTORY DETECTED: Racer " << physics.winnerIndex << ". Finishing recording..." << std::endl;
                victorySequenceStarted = true;
            }
            victoryTimer += dtSec;
            if (victoryTimer >= VICTORY_DELAY) {
                std::cout << ">>> CLOSING SIMULATION." << std::endl;
                if (recorder) recorder->stop();
                window.close();
            }
        }

        // ==============================================
        // --- UI ESTILO UNITY/UE5 ---
        // ==============================================

        // 1. TOOLBAR (Panel Superior)
    // 1. TOOLBAR (Panel Superior, anclado arriba y ocupando el ancho del centro)
        ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        if (!isMobile) panelFlags |= ImGuiWindowFlags_NoCollapse; // Dejamos colapsar en celular

        ImGui::SetNextWindowPos(ImVec2(isMobile ? 0 : leftPanelWidth, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(isMobile ? screenWidth : availableWidth, toolbarHeight), ImGuiCond_Always);
ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);

        // START/STOP REC
        if (recorder && recorder->isRecording) {
            ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.6f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0.0f, 0.7f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0.0f, 0.8f, 0.8f));
            if (ImGui::Button("STOP REC", ImVec2(100, isMobile ? 45 : 35))) {
                recorder->stop();
                recorder.reset();
            }
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.33f, 0.6f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0.33f, 0.7f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0.33f, 0.8f, 0.8f));
            if (ImGui::Button("START REC", ImVec2(100, isMobile ? 45 : 35))) {
#ifndef __ANDROID__
                recorder = std::make_unique<Recorder>(RENDER_WIDTH, RENDER_HEIGHT, recordFPS, VIDEO_DIRECTORY);
                soundManager.setRecorder(recorder.get());
                recorder->isRecording = true;
#else
                std::cout << ">>> Grabacion en Android deshabilitada por ahora bo." << std::endl;
#endif
            }
            ImGui::PopStyleColor(3);
        }

        if (!isMobile) ImGui::SameLine();

        // CONTROLES DE FPS
        ImGui::SetNextItemWidth(100);
        if (ImGui::SliderInt("Sim FPS", &simFPS, 30, 240)) window.setFramerateLimit(simFPS);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderInt("Rec FPS", &recordFPS, 30, 240);

        if (!isMobile) ImGui::SameLine();

        // CONTROLES DE FÍSICA
        if (physics.isPaused) {
            if (ImGui::Button("RESUME", ImVec2(90, isMobile ? 45 : 35))) physics.isPaused = false;
        } else {
            if (ImGui::Button("PAUSE", ImVec2(90, isMobile ? 45 : 35))) physics.isPaused = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("RESET RACE", ImVec2(100, isMobile ? 45 : 35))) {
            physics.resetRacers();
            for(auto& t : trails) t.points.clear();
            victoryTimer = 0.0f;
            victorySequenceStarted = false;
        }

        if (!isMobile) ImGui::SameLine();

        // CANCIÓN
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##SongFile", songFile, 128);
        ImGui::SameLine();
        if (ImGui::Button("LOAD SONG", ImVec2(90, isMobile ? 45 : 35))) physics.loadSong(songFile);

        ImGui::End();

        // 2. HIERARCHY (Panel Izquierdo)
        ImGui::SetNextWindowPos(ImVec2(0, isMobile ? screenHeight - 200 : 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(leftPanelWidth, isMobile ? 200 : screenHeight), ImGuiCond_Always);
        ImGui::Begin("Hierarchy", nullptr, panelFlags);

        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1), "SCENE");
        if (ImGui::Selectable("Global Settings", selectedType == EntityType::Global)) selectedType = EntityType::Global;
        if (ImGui::Selectable("Win Zone", selectedType == EntityType::WinZone)) selectedType = EntityType::WinZone;
        if (ImGui::Selectable("Racers Fleet", selectedType == EntityType::Racers)) selectedType = EntityType::Racers;

        ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 0.0f, 1, 1), "POST-PROCESSING");
            ImGui::Checkbox("Enable Neon Bloom", &enableBloom);
            if (enableBloom) {
                ImGui::Indent();
                ImGui::DragFloat("Threshold", &bloomThreshold, 0.05f, 0.0f, 1.0f);
                ImGui::DragFloat("Intensity", &bloomMultiplier, 0.05f, 0.0f, 5.0f);
                ImGui::SliderInt("Glow Spread", &blurIterations, 1, 8);
                ImGui::Unindent();
            }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "WALLS");
        ImGui::SameLine(ImGui::GetWindowWidth() - 35);
        if (ImGui::Button("+", ImVec2(25, 20))) {
            physics.addCustomWall(12.0f, 20.0f, 10.0f, 1.0f, 1);
            selectedType = EntityType::Wall;
            selectedIndex = (int)physics.getCustomWalls().size() - 1;
        }

        const auto& walls = physics.getCustomWalls();
        for (int i = 0; i < (int)walls.size(); ++i) {
            std::string label = "Wall " + std::to_string(i);
            if (walls[i].soundID > 0) label += " [S]"; // ♪ Indica que tiene sonido asignado
            if (walls[i].isExpandable) label += " [E]";
            if (walls[i].isMoving) label += " [M]";
            if (walls[i].shapeType == 1) label += " [Spike]";

            if (ImGui::Selectable(label.c_str(), selectedType == EntityType::Wall && selectedIndex == i)) {
                selectedType = EntityType::Wall;
                selectedIndex = i;
            }
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "WEAPONS");
        ImGui::SameLine(ImGui::GetWindowWidth() - 35);
        if (ImGui::Button("+##Knife", ImVec2(25, 20))) {
            physics.addKnife(12.0f, 12.0f); // Spawnea en el medio
            selectedType = EntityType::Knife;
            selectedIndex = (int)physics.getKnives().size() - 1;
        }

        const auto& knives = physics.getKnives();
        for (int i = 0; i < (int)knives.size(); ++i) {
            std::string label = "Knife " + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), selectedType == EntityType::Knife && selectedIndex == i)) {
                selectedType = EntityType::Knife;
                selectedIndex = i;
            }
        }

        ImGui::End();

        // 3. INSPECTOR (Panel Derecho, de arriba a abajo)
        ImGui::SetNextWindowPos(ImVec2(screenWidth - rightPanelWidth, isMobile ? toolbarHeight : 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(rightPanelWidth, isMobile ? screenHeight/2 : screenHeight), ImGuiCond_Always);
        ImGui::Begin("Inspector", nullptr, panelFlags);

        int wallToDelete = -1;
        int wallToDuplicate = -1;

        if (selectedType == EntityType::None) {
            ImGui::TextDisabled("Select an object\nin the Hierarchy.");
        }
        else if (selectedType == EntityType::Global) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1), "MAP SYSTEM");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##Filename", mapFilename, IM_ARRAYSIZE(mapFilename));
#ifndef __ANDROID__
            if (ImGui::Button("SAVE MAP", ImVec2(-1, 30))) physics.saveMap(mapFilename);
#endif
            if (ImGui::Button("LOAD MAP", ImVec2(-1, 30))) {
                physics.loadMap(mapFilename);
                for(auto& t : trails) t.points.clear();
                selectedType = EntityType::None; // Reset selection safety
            }

            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "GLOBAL PHYSICS");
            ImGui::Checkbox("Gravity", &physics.enableGravity);
            ImGui::Checkbox("Stop on First Win", &physics.stopOnFirstWin);
            ImGui::DragFloat("Finish Delay (s)", &physics.finishDelay, 0.05f, 0.0f, 2.0f);



            ImGui::Checkbox("Chaos Mode", &physics.enableChaos);
            if (physics.enableChaos) {
                ImGui::Indent();
                ImGui::SliderFloat("Chaos %", &physics.chaosChance, 0.0f, 0.5f);
                ImGui::SliderFloat("Boost", &physics.chaosBoost, 1.0f, 3.0f);
                ImGui::Unindent();
            }
        }
        else if (selectedType == EntityType::WinZone) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "WIN ZONE CONFIG");
            bool updateZone = false;
            updateZone |= ImGui::DragFloat2("Pos", physics.winZonePos, 0.1f);
            updateZone |= ImGui::DragFloat2("Size", physics.winZoneSize, 0.1f);
            if (updateZone) physics.updateWinZone(physics.winZonePos[0], physics.winZonePos[1], physics.winZoneSize[0], physics.winZoneSize[1]);

            ImGui::Checkbox("Enable Neon Pulse (Glow)", &physics.winZoneGlow);
        }
        else if (selectedType == EntityType::Knife) {
            if (selectedIndex >= 0 && selectedIndex < physics.getKnives().size()) {
                auto& k = physics.getKnives()[selectedIndex];

                ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "KNIFE %d", selectedIndex);

                float pos[2] = { k.initialPos.x, k.initialPos.y };
                if (ImGui::DragFloat2("Position", pos, 0.1f)) {
                    physics.updateKnifePos(selectedIndex, pos[0], pos[1]);
                }

                if (k.isPickedUp) {
                    ImGui::TextDisabled("Currently held by Racer %d", k.ownerIndex);
                } else {
                    ImGui::TextDisabled("Currently on the ground.");
                }

                ImGui::Separator();
                ImGui::Spacing();

                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0.0f, 0.7f, 0.7f));
                if (ImGui::Button("DELETE KNIFE", ImVec2(-1, 30))) {
                    physics.removeKnife(selectedIndex);
                    selectedType = EntityType::None;
                }
                ImGui::PopStyleColor(2);
            } else {
                selectedType = EntityType::None;
            }
        }
        else if (selectedType == EntityType::Racers) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 1.0f, 1), "FLEET SETTINGS");
            float size = physics.currentRacerSize;
            if (ImGui::DragFloat("Size", &size, 0.05f, 0.1f, 10.0f)) physics.updateRacerSize(size);
            float rest = physics.currentRestitution;
            if (ImGui::DragFloat("Bounce", &rest, 0.01f, 0.0f, 2.0f)) physics.updateRestitution(rest);
            bool fixedRot = physics.currentFixedRotation;
            if (ImGui::Checkbox("Fixed Rotation", &fixedRot)) physics.updateFixedRotation(fixedRot);
            ImGui::Checkbox("Enforce Speed", &physics.enforceSpeed);
            ImGui::DragFloat("Target Vel", &physics.targetSpeed, 0.5f, 0.0f, 100.0f);

            ImGui::Separator();
            ImGui::Text("INDIVIDUAL RACERS");
            for (size_t i = 0; i < bodies.size(); ++i) {
                b2Body* b = bodies[i];
                ImGui::PushStyleColor(ImGuiCol_Header, guiColors[i % 4]);
                ImGui::PushID((int)i);
                std::string headerName = (i < 4) ? std::string(racerNames[i]) : "Racer " + std::to_string(i);
                if (ImGui::CollapsingHeader(headerName.c_str())) {
                    b2Vec2 pos = b->GetPosition(); float p[2] = { pos.x, pos.y };
                    if (ImGui::DragFloat2("Pos", p, 0.1f)) { b->SetTransform(b2Vec2(p[0], p[1]), b->GetAngle()); b->SetAwake(true); }
                    b2Vec2 vel = b->GetLinearVelocity(); float v[2] = { vel.x, vel.y };
                    if (ImGui::DragFloat2("Vel", v, 0.1f)) { b->SetLinearVelocity(b2Vec2(v[0], v[1])); b->SetAwake(true); }
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
        else if (selectedType == EntityType::Wall) {
            if (selectedIndex >= 0 && selectedIndex < physics.getCustomWalls().size()) {
                CustomWall& w = physics.getCustomWalls()[selectedIndex];

                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "WALL %d", selectedIndex);

                float pos[2] = { w.body->GetPosition().x, w.body->GetPosition().y };
                float size[2] = { w.width, w.height };
                int snd = w.soundID;

                bool changed = false;
                changed |= ImGui::DragFloat2("Position", pos, 0.1f);
                changed |= ImGui::DragFloat2("Size", size, 0.1f, 0.5f, 30.0f);
                changed |= ImGui::SliderInt("Sound ID", &snd, 0, 8);

                // --- CONTROL DE CAPAS Y ESTILO INDEPENDIENTE ---
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "RENDER LAYER & STYLE");

                ImGui::InputInt("Z-Index Layer", &w.zIndex, 1, 5);

                // El checkbox para matar el borde a esta pared específica
                ImGui::Checkbox("Draw Outline (Border)", &w.hasOutline);

                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "DESTRUCTION SYSTEM");
                if (ImGui::Checkbox("Is Destructible", &w.isDestructible)) {
                    if (w.isDestructible) w.currentHits = w.maxHits;
                }

                if (w.isDestructible) {
                    ImGui::Indent();
                    int oldMax = w.maxHits;
                    if (ImGui::SliderInt("Max Hits", &w.maxHits, 1, 200)) {
                        // FIX LÓGICO: Si alteramos el máximo, ajustamos la vida actual en caliente.
                        if (w.currentHits == oldMax) w.currentHits = w.maxHits;
                        else if (w.currentHits > w.maxHits) w.currentHits = w.maxHits;
                    }

                    // Slider explícito de vida para control total
                    ImGui::SliderInt("Current Hits", &w.currentHits, 1, w.maxHits);

                    // --- NUEVO: TOGGLE TEXTO / LEDS ---
                    ImGui::Checkbox("Use Text for HP (Instead of LEDs)", &w.useTextForHP);

                    float healthPct = (float)w.currentHits / (float)w.maxHits;
                    std::string hpOverlay = std::to_string(w.currentHits) + " / " + std::to_string(w.maxHits);
                    ImGui::ProgressBar(healthPct, ImVec2(-1, 0), hpOverlay.c_str());
                    ImGui::Unindent();
                }

                if (changed) physics.updateCustomWall(selectedIndex, pos[0], pos[1], size[0], size[1], snd, w.shapeType, w.rotation);

                ImGui::Separator();
// --- GEOMETRÍA ---
                if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
                    int sType = w.shapeType;
                    float rotDeg = w.rotation * 180.0f / 3.14159f;
                    bool geoChanged = false;

                    if (ImGui::RadioButton("Box", sType == 0)) { sType = 0; geoChanged = true; } ImGui::SameLine();
                    if (ImGui::RadioButton("Spike", sType == 1)) { sType = 1; geoChanged = true; w.isDeadly = true; }

                    if (ImGui::SliderFloat("Rotation", &rotDeg, 0.0f, 360.0f, "%.0f deg")) geoChanged = true;

                    // Botones de rotación rápida alineados prolijamente
                    if (ImGui::Button("0°", ImVec2(40,0))) { rotDeg = 0.0f; geoChanged = true; } ImGui::SameLine();
                    if (ImGui::Button("90°", ImVec2(40,0))) { rotDeg = 90.0f; geoChanged = true; } ImGui::SameLine();
                    if (ImGui::Button("180°", ImVec2(45,0))) { rotDeg = 180.0f; geoChanged = true; } ImGui::SameLine();
                    if (ImGui::Button("270°", ImVec2(45,0))) { rotDeg = 270.0f; geoChanged = true; }

                    if (geoChanged || changed) {
                        float rotRad = rotDeg * 3.14159f / 180.0f;
                        physics.updateCustomWall(selectedIndex, pos[0], pos[1], size[0], size[1], snd, sType, rotRad);
                    }
                }

                // --- APARIENCIA ---
                if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen)) {
                    sf::Color c = w.neonColor;
                    ImVec4 imColor = ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);

                    ImGui::ColorButton("##preview", imColor, ImGuiColorEditFlags_NoTooltip, ImVec2(24, 24));
                    ImGui::SameLine();

                    int currentColorIdx = w.colorIndex;
                    const char* colorNames[] = { "Cyan", "Magenta", "Lime", "Orange", "Purple", "Red", "Gold", "Blue", "Pink" };
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##Color", &currentColorIdx, colorNames, IM_ARRAYSIZE(colorNames))) {
                        physics.updateWallColor(selectedIndex, currentColorIdx);
                    }
                }

                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "DANGER ZONE");
                if (ImGui::Checkbox("IS DEADLY (Spike)", &w.isDeadly)) {
                    if (w.isDeadly) physics.updateWallColor(selectedIndex, 5);
                }

                ImGui::Separator();
                if (ImGui::CollapsingHeader("Expansion Properties")) {
                    ImGui::Checkbox("Is Expandable", &w.isExpandable);
                    if (w.isExpandable) {
                        ImGui::Indent();
                        ImGui::DragFloat("Start Delay", &w.expansionDelay, 0.1f, 0.0f, 60.0f);
                        ImGui::DragFloat("Speed", &w.expansionSpeed, 0.05f, 0.01f, 10.0f);
                        ImGui::RadioButton("X", &w.expansionAxis, 0); ImGui::SameLine();
                        ImGui::RadioButton("Y", &w.expansionAxis, 1); ImGui::SameLine();
                        ImGui::RadioButton("XY", &w.expansionAxis, 2);
                        ImGui::Checkbox("Stop on Contact", &w.stopOnContact);
                        if (w.stopOnContact) ImGui::InputInt("Target Wall", &w.stopTargetIdx);
                        ImGui::DragFloat("Max Size", &w.maxSize, 0.5f, 0.0f, 100.0f);
                        ImGui::Unindent();
                    }
                }

                if (ImGui::CollapsingHeader("Kinematic Movement")) {
                    if (ImGui::Checkbox("Is Moving Platform", &w.isMoving)) {
                        if (w.isMoving) {
                            w.pointA = w.body->GetPosition();
                            w.pointB = w.pointA + b2Vec2(5.0f, 0.0f);
                            w.body->SetType(b2_kinematicBody);
                        } else {
                            w.body->SetType(b2_staticBody);
                            w.body->SetLinearVelocity(b2Vec2(0.0f, 0.0f));
                        }
                    }

                    if (w.isMoving) {
                        ImGui::Indent();
                        float pA[2] = { w.pointA.x, w.pointA.y };
                        if (ImGui::DragFloat2("Point A", pA, 0.1f)) w.pointA.Set(pA[0], pA[1]);

                        float pB[2] = { w.pointB.x, w.pointB.y };
                        if (ImGui::DragFloat2("Point B", pB, 0.1f)) w.pointB.Set(pB[0], pB[1]);

                        ImGui::DragFloat("Speed", &w.moveSpeed, 0.1f, 0.1f, 50.0f);

                        if (ImGui::Button("Set A = Current", ImVec2(-1, 0))) w.pointA = w.body->GetPosition();
                        if (ImGui::Button("Set B = Current", ImVec2(-1, 0))) w.pointB = w.body->GetPosition();

                        ImGui::Checkbox("Reverse on Wall", &w.reverseOnContact);
                        if (w.reverseOnContact) {
                            ImGui::Checkbox("Free Bounce", &w.freeBounce);
                            if (w.isFreeBouncing && ImGui::Button("Reset Route")) {
                                w.isFreeBouncing = false;
                                w.body->SetLinearVelocity(b2Vec2(0.0f, 0.0f));
                            }
                        }
                        ImGui::Unindent();
                    }
                }

                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button("DUPLICATE", ImVec2(-1, 30))) wallToDuplicate = selectedIndex;
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0.0f, 0.7f, 0.7f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0.0f, 0.8f, 0.8f));
                if (ImGui::Button("DELETE", ImVec2(-1, 30))) wallToDelete = selectedIndex;
                ImGui::PopStyleColor(3);

            } else {
                selectedType = EntityType::None; // Safe fallback si se borró y quedó desfasado el índice
            }
        }

        ImGui::End();

        // Procesamiento de comandos de la UI (Borrar / Duplicar)
        if (wallToDelete != -1) {
            physics.removeCustomWall(wallToDelete);
            selectedType = EntityType::None; // Deseleccionamos por seguridad
        }
        if (wallToDuplicate != -1) {
            physics.duplicateCustomWall(wallToDuplicate);
            selectedType = EntityType::Wall;
            selectedIndex = (int)physics.getCustomWalls().size() - 1; // Seleccionamos el clon nuevo
        }

        // ==============================================
        // --- DRAW: RENDERIZADO AL BUFFER GIGANTE ---
        // ==============================================
        // 1. Limpiar con el color de vacío
        gameBuffer.clear(sf::Color(30, 30, 30));

        // 2. Dibujar Polvo Atmosférico con movimiento energético
        sf::VertexArray dustVA(sf::PrimitiveType::Triangles, ambientDust.size() * 6);
        for(size_t i = 0; i < ambientDust.size(); ++i) {
            auto& p = ambientDust[i];

            if (!physics.isPaused || (recorder && recorder->isRecording)) {
                p.yPos += p.speedY * dtSec;
                if (p.yPos < -50.0f) {
                    p.yPos = RENDER_HEIGHT + 50.0f;
                    p.basePos.x = (float)(std::rand() % RENDER_WIDTH);
                }
            }

            float currentX = p.basePos.x + std::sin(globalTime * p.phaseSpeed + p.phaseOffset) * p.amplitude;
            float s = p.size;

            sf::Vector2f tl(currentX - s, p.yPos - s);
            sf::Vector2f tr(currentX + s, p.yPos - s);
            sf::Vector2f br(currentX + s, p.yPos + s);
            sf::Vector2f bl(currentX - s, p.yPos + s);

            dustVA[i*6 + 0] = sf::Vertex{tl, p.color};
            dustVA[i*6 + 1] = sf::Vertex{tr, p.color};
            dustVA[i*6 + 2] = sf::Vertex{br, p.color};
            dustVA[i*6 + 3] = sf::Vertex{br, p.color};
            dustVA[i*6 + 4] = sf::Vertex{bl, p.color};
            dustVA[i*6 + 5] = sf::Vertex{tl, p.color};
        }

        sf::RenderStates dustStates;
        dustStates.blendMode = sf::BlendAdd;
        gameBuffer.draw(dustVA, dustStates);

        // 3. Dibujar la grilla encima
        gameBuffer.draw(background);


        // ==============================================
        // --- SISTEMA DE CAPAS (Z-INDEX LOOP) ---
        // ==============================================
        const auto& customWalls = physics.getCustomWalls();

        // Calculamos los extremos del bucle dinámicamente
        int minLayer = -1; // Nos aseguramos que al menos pase por el -1
        int maxLayer = -1;
        for (const auto& wall : customWalls) {
            if (wall.zIndex < minLayer) minLayer = wall.zIndex;
            if (wall.zIndex > maxLayer) maxLayer = wall.zIndex;
        }

        for (int currentLayer = minLayer; currentLayer <= maxLayer; ++currentLayer) {

            // ------------------------------------------
            // CAPA -1: ENTIDADES Y SIMULACIÓN BASE
            // ------------------------------------------
            if (currentLayer == -1) {

                // --- A) WINZONE ---
                b2Body* zone = physics.getWinZoneBody();
                if (zone) {
                    b2Vec2 pos = zone->GetPosition();
                    sf::RectangleShape zoneRect;
                    float w = physics.winZoneSize[0] * physics.SCALE;
                    float h = physics.winZoneSize[1] * physics.SCALE;

                    float alpha = 100.0f;
                    if (physics.winZoneGlow) {
                        float pulse = (std::sin(globalTime * 1.5f) + 1.0f) * 0.5f;
                        alpha = 50.0f + pulse * 100.0f;
                    }

                    zoneRect.setSize({w, h});
                    zoneRect.setOrigin({w/2.0f, h/2.0f});
                    zoneRect.setPosition({pos.x * physics.SCALE, pos.y * physics.SCALE});
                    zoneRect.setFillColor(sf::Color(255, 215, 0, (std::uint8_t)alpha));
                    zoneRect.setOutlineColor(sf::Color::Yellow);
                    zoneRect.setOutlineThickness(0.1f * physics.SCALE);
                    gameBuffer.draw(zoneRect);
                }

                // --- B) CUCHILLOS ---
                for (const auto& knife : physics.getKnives()) {
                    sf::Vector2f drawPos;
                    float drawRot;
                    float kScale = 1.5f * physics.SCALE;

                    if (!knife.isPickedUp) {
                        drawPos = sf::Vector2f(knife.body->GetPosition().x * physics.SCALE, knife.body->GetPosition().y * physics.SCALE);
                        drawRot = knife.body->GetAngle() * 180.0f / 3.14159f;
                    } else {
                        int oIdx = knife.ownerIndex;
                        if (oIdx >= 0 && oIdx < bodies.size()) {
                            b2Body* ownerBody = bodies[oIdx];
                            b2Vec2 oPos = ownerBody->GetPosition();
                            float oAngle = ownerBody->GetAngle();

                            float rSize = physics.currentRacerSize / 2.0f;
                            b2Vec2 offset(cos(oAngle) * (rSize + 0.3f), sin(oAngle) * (rSize + 0.3f));

                            drawPos = sf::Vector2f((oPos.x + offset.x) * physics.SCALE, (oPos.y + offset.y) * physics.SCALE);
                            drawRot = oAngle * 180.0f / 3.14159f;
                        } else {
                            continue;
                        }
                    }

                    if (hasKnifeTex) {
                        sf::Sprite s(knifeTex);
                        s.setOrigin({(float)knifeTex.getSize().x / 2.0f, (float)knifeTex.getSize().y / 2.0f});
                        float scaleFactor = kScale / knifeTex.getSize().x;
                        s.setScale({-scaleFactor, scaleFactor});
                        s.setPosition(drawPos);
                        s.setRotation(sf::degrees(drawRot));
                        gameBuffer.draw(s);
                    } else {
                        sf::ConvexShape tri;
                        tri.setPointCount(3);
                        float triSize = kScale * 0.6f;
                        tri.setPoint(0, {0.0f, -triSize});
                        tri.setPoint(1, {triSize/2.0f, triSize/2.0f});
                        tri.setPoint(2, {-triSize/2.0f, triSize/2.0f});
                        tri.setPosition(drawPos);
                        tri.setRotation(sf::degrees(drawRot + 90.0f));
                        tri.setFillColor(sf::Color(220, 220, 220));
                        tri.setOutlineColor(sf::Color::Red);
                        tri.setOutlineThickness(2.0f);
                        gameBuffer.draw(tri);
                    }
                }

                // --- C) TUMBAS ---
                const auto& statuses = physics.getRacerStatus();
                float tombSize = 0.8f * physics.SCALE;
                float crossThick = 0.15f * physics.SCALE;
                float outlineThick = 0.08f * physics.SCALE;

                for (size_t i = 0; i < statuses.size(); ++i) {
                    const auto& status = statuses[i];
                    if (!status.isAlive) {
                        float px = status.deathPos.x * physics.SCALE;
                        float py = status.deathPos.y * physics.SCALE;
                        sf::Color deathColor = (i < 4) ? racerColors[i] : sf::Color::White;

                        sf::RectangleShape grave;
                        grave.setSize({tombSize, tombSize});
                        grave.setOrigin({tombSize / 2.0f, tombSize / 2.0f});
                        grave.setPosition({px, py});
                        grave.setFillColor(sf::Color(20, 20, 20, 240));
                        grave.setOutlineColor(deathColor);
                        grave.setOutlineThickness(outlineThick);
                        gameBuffer.draw(grave);

                        float crossLen = tombSize * 0.8f;
                        sf::RectangleShape bar1({crossLen, crossThick});
                        sf::RectangleShape bar2({crossLen, crossThick});
                        bar1.setOrigin({crossLen / 2.0f, crossThick / 2.0f});
                        bar2.setOrigin({crossLen / 2.0f, crossThick / 2.0f});
                        bar1.setPosition({px, py});
                        bar2.setPosition({px, py});
                        bar1.setRotation(sf::degrees(45.0f));
                        bar2.setRotation(sf::degrees(-45.0f));
                        bar1.setFillColor(deathColor);
                        bar2.setFillColor(deathColor);
                        gameBuffer.draw(bar1);
                        gameBuffer.draw(bar2);
                    }
                }

                // --- D) TRAILS ---
                for (size_t i = 0; i < trails.size(); ++i) {
                    const auto& pts = trails[i].points;
                    if (pts.size() < 2) continue;

                    sf::VertexArray glowVA(sf::PrimitiveType::Triangles);
                    sf::VertexArray coreVA(sf::PrimitiveType::Triangles);
                    float baseWidth = physics.currentRacerSize * physics.SCALE;

// Dentro del for de dibujado de estelas:
        for (size_t j = 1; j < pts.size(); ++j) {
            // Agregá ".pos" porque ahora es una estructura compuesta
            sf::Vector2f p1 = pts[j-1].pos;
            sf::Vector2f p2 = pts[j].pos;
            // ... el resto de la matemática queda todo exactamente igual.

                        sf::Vector2f dir = p2 - p1;
                        float len = std::sqrt(dir.x*dir.x + dir.y*dir.y);
                        if (len < 0.001f) continue;

                        sf::Vector2f normal(-dir.y/len, dir.x/len);

                        // MAGIA ACÁ: Desacoplamos visualmente de la cantidad de puntos.
                        // Usamos el TIEMPO real del punto. Queda idéntico sin importar los FPS.
                        float maxAge = std::max(0.001f, trails[i].currentDuration);
                        float age1 = globalTime - pts[j-1].time;
                        float age2 = globalTime - pts[j].time;

                        float lifePct1 = 1.0f - (age1 / maxAge);
                        float lifePct2 = 1.0f - (age2 / maxAge);

                        // Clamp manual para blindar errores si algún punto de la física se atrasa
                        if (lifePct1 < 0.0f) lifePct1 = 0.0f; else if (lifePct1 > 1.0f) lifePct1 = 1.0f;
                        if (lifePct2 < 0.0f) lifePct2 = 0.0f; else if (lifePct2 > 1.0f) lifePct2 = 1.0f;

                        float widthPct1 = std::pow(lifePct1, 0.6f);
                        float widthPct2 = std::pow(lifePct2, 0.6f);
                        sf::Color baseColor = trails[i].color;

                        auto getThermoColor = [&](float life, float alphaMult) -> sf::Color {
                            sf::Color c;
                            if (life >= 0.8f) {
                                c = sf::Color(245, 245, 245);
                            } else if (life >= 0.3f) {
                                float t = (life - 0.3f) / 0.5f;
                                c = lerpColor(baseColor, sf::Color(245, 245, 245), t);
                            } else {
                                float t = life / 0.3f;
                                sf::Color transparent(0, 0, 0, 0);
                                c = lerpColor(transparent, baseColor, t);
                            }
                            c.a = (std::uint8_t)(c.a * alphaMult);
                            return c;
                        };

                        sf::Color coreColor1 = getThermoColor(lifePct1, 0.85f);
                        sf::Color coreColor2 = getThermoColor(lifePct2, 0.85f);
                        sf::Color glowColor1 = getThermoColor(lifePct1, 0.35f);
                        sf::Color glowColor2 = getThermoColor(lifePct2, 0.35f);

                        float coreW1 = (baseWidth * 0.3f) * widthPct1;
                        float coreW2 = (baseWidth * 0.3f) * widthPct2;
                        float glowWidth = baseWidth * 1.6f;
                        float glowW1 = (glowWidth * 0.4f) * widthPct1;
                        float glowW2 = (glowWidth * 0.4f) * widthPct2;

                        sf::Vector2f overlap = (dir / len) * (baseWidth * 0.08f);
                        sf::Vector2f p1_ext = p1 - overlap;
                        sf::Vector2f p2_ext = p2 + overlap;

            sf::Vertex c0{p1_ext + normal * coreW1, coreColor1};
            sf::Vertex c1{p1_ext - normal * coreW1, coreColor1};
            sf::Vertex c2{p2_ext - normal * coreW2, coreColor2};
            sf::Vertex c3{p2_ext + normal * coreW2, coreColor2};

            coreVA.append(c0); coreVA.append(c1); coreVA.append(c2);
            coreVA.append(c2); coreVA.append(c3); coreVA.append(c0);

            sf::Vertex g0{p1_ext + normal * glowW1, glowColor1};
            sf::Vertex g1{p1_ext - normal * glowW1, glowColor1};
            sf::Vertex g2{p2_ext - normal * glowW2, glowColor2};
            sf::Vertex g3{p2_ext + normal * glowW2, glowColor2};

            glowVA.append(g0); glowVA.append(g1); glowVA.append(g2);
            glowVA.append(g2); glowVA.append(g3); glowVA.append(g0);
                    }

                    sf::RenderStates states;
                    states.blendMode = sf::BlendAdd;
                    gameBuffer.draw(glowVA, states);
                    gameBuffer.draw(coreVA, states);
                }

                // --- E) RACERS VIVOS ---
                const auto& currentStatuses = physics.getRacerStatus();
                for (size_t i = 0; i < bodies.size(); ++i) {
                    if (i < currentStatuses.size() && !currentStatuses[i].isAlive) continue;
                    b2Body* body = bodies[i];
                    b2Vec2 pos = body->GetPosition();
                    float angle = body->GetAngle();
                    float drawSize = physics.currentRacerSize * physics.SCALE;

                    sf::RectangleShape rect;
                    rect.setSize({drawSize, drawSize});
                    rect.setOrigin({drawSize / 2.0f, drawSize / 2.0f});
                    rect.setPosition({pos.x * physics.SCALE, pos.y * physics.SCALE});
                    rect.setRotation(sf::degrees(angle * 180.0f / 3.14159f));
                    if (i < 4) rect.setOutlineColor(racerColors[i]);
                    else rect.setOutlineColor(sf::Color::White);

                    rect.setFillColor(sf::Color::White);
                    rect.setOutlineThickness(-0.1f * physics.SCALE);
                    gameBuffer.draw(rect);
                }
            } // FIN CAPA -1

            // ------------------------------------------
            // CAPAS > -1: PAREDES CUSTOM
            // ------------------------------------------
            for (const auto& wall : customWalls) {
                if (wall.zIndex != currentLayer) continue;

                b2Vec2 pos = wall.body->GetPosition();
                float wPx = wall.width * physics.SCALE;
                float hPx = wall.height * physics.SCALE;

                sf::Shape* shapeToDraw = nullptr;
                sf::RectangleShape rectShape;
                sf::ConvexShape triShape;

                if (wall.shapeType == 1) {
                    triShape.setPointCount(3);
                    triShape.setPoint(0, {0.0f, -hPx / 2.0f});
                    triShape.setPoint(1, {wPx / 2.0f, hPx / 2.0f});
                    triShape.setPoint(2, {-wPx / 2.0f, hPx / 2.0f});
                    triShape.setPosition({pos.x * physics.SCALE, pos.y * physics.SCALE});
                    triShape.setRotation(sf::degrees(wall.body->GetAngle() * 180.0f / 3.14159f));
                    shapeToDraw = &triShape;
                } else {
                    rectShape.setSize({wPx, hPx});
                    rectShape.setOrigin({wPx / 2.0f, hPx / 2.0f});
                    rectShape.setPosition({pos.x * physics.SCALE, pos.y * physics.SCALE});
                    rectShape.setRotation(sf::degrees(wall.body->GetAngle() * 180.0f / 3.14159f));
                    shapeToDraw = &rectShape;
                }

                sf::Color currentFill = lerpColor(wall.baseFillColor, wall.flashColor, wall.flashTimer);
                sf::Color currentOutline = lerpColor(wall.neonColor, sf::Color::White, wall.flashTimer * 0.5f);

                if (wall.isDeadly) {
                    float dangerPulse = (std::sin(globalTime * 10.0f) + 1.0f) * 0.5f;
                    currentFill = sf::Color(100 + (dangerPulse * 50), 0, 0, 255);
                    currentOutline = sf::Color::Red;
                }

                shapeToDraw->setFillColor(currentFill);
                shapeToDraw->setOutlineColor(currentOutline);

                if (wall.borderSide != -1 || !wall.hasOutline) {
                    shapeToDraw->setOutlineThickness(0.0f);
                } else {
                    float baseThickness = 0.08f * physics.SCALE;
                    float thickness = baseThickness + (wall.flashTimer * baseThickness);
                    shapeToDraw->setOutlineThickness(-thickness);
                }

                gameBuffer.draw(*shapeToDraw);

                // --- RENDERIZADO DE DAÑO Y VIDA ---
                if (wall.isDestructible) {
                    float halfW = wPx / 2.0f;
                    float halfH = hPx / 2.0f;

                    if (wall.currentHits < wall.maxHits) {
                        int damageLevel = wall.maxHits - wall.currentHits;
                        int numCracks = std::min(damageLevel, 200);
                        std::srand((unsigned int)(reinterpret_cast<std::uintptr_t>(wall.body) & 0xFFFFFFFF));

                        auto drawCrackSegment = [&](float x1, float y1, float x2, float y2) {
                            float dx = x2 - x1;
                            float dy = y2 - y1;
                            float length = std::sqrt(dx*dx + dy*dy);
                            if (length < 0.5f) return;

                            float crackAngle = std::atan2(dy, dx) * 180.0f / 3.14159f;
                            float crackThickness = std::max(4.0f, 0.036f * physics.SCALE);

                            sf::RectangleShape crackRect({length, crackThickness});
                            crackRect.setOrigin({0.0f, crackThickness / 2.0f});
                            crackRect.setFillColor(sf::Color(10, 10, 10, 220));

                            sf::Transform t;
                            t.translate({pos.x * physics.SCALE, pos.y * physics.SCALE});
                            t.rotate(sf::degrees(wall.body->GetAngle() * 180.0f / 3.14159f));

                            crackRect.setPosition(t.transformPoint({x1, y1}));
                            crackRect.setRotation(sf::degrees((wall.body->GetAngle() * 180.0f / 3.14159f) + crackAngle));
                            gameBuffer.draw(crackRect);
                        };

                        for (int k = 0; k < numCracks; k++) {
                            float currentX = (std::rand() % (int)wPx) - halfW;
                            float currentY = (std::rand() % (int)hPx) - halfH;
                            float baseAngle = (std::rand() % 360) * 3.14159f / 180.0f;
                            int segments = 2 + (std::rand() % 3);

                            for (int s = 0; s < segments; s++) {
                                float angle = baseAngle + ((std::rand() % 100) - 50) * 0.015f;
                                float maxDim = std::max(wPx, hPx);
                                float segLen = maxDim * (0.05f + (std::rand() % 100) * 0.002f);

                                float nextX = currentX + std::cos(angle) * segLen;
                                float nextY = currentY + std::sin(angle) * segLen;

                                nextX = std::clamp(nextX, -halfW, halfW);
                                nextY = std::clamp(nextY, -halfH, halfH);

                                drawCrackSegment(currentX, currentY, nextX, nextY);

                                currentX = nextX;
                                currentY = nextY;
                            }
                        }
                        std::srand(std::time(nullptr));
                    }

                    if (wall.useTextForHP) {
                        sf::Text hitText(uiFont);
                        hitText.setString(std::to_string(wall.currentHits));

                        float minDim = std::min(wPx, hPx);
                        unsigned int calcSize = (unsigned int)(minDim * 0.6f);
                        if (calcSize < 12) calcSize = 12;
                        hitText.setCharacterSize(calcSize);
                        hitText.setFillColor(sf::Color(255, 255, 255, 140));

                        sf::FloatRect textRect = hitText.getLocalBounds();
                        hitText.setOrigin({textRect.position.x + textRect.size.x / 2.0f, textRect.position.y + textRect.size.y / 2.0f});
                        hitText.setPosition({pos.x * physics.SCALE, pos.y * physics.SCALE});

                        float extraRotation = (hPx > wPx * 1.5f) ? 90.0f : 0.0f;
                        hitText.setRotation(sf::degrees(wall.body->GetAngle() * 180.0f / 3.14159f + extraRotation));
                        gameBuffer.draw(hitText);
                    } else {
                        float ledBaseSize = 0.30f * physics.SCALE;
                        float spacing = 0.12f * physics.SCALE;
                        bool vertical = (wPx < hPx);
                        float mainLength = vertical ? hPx : wPx;
                        float totalWidth = (wall.maxHits * ledBaseSize) + ((wall.maxHits - 1) * spacing);
                        float scaleDown = 1.0f;
                        if (totalWidth > mainLength * 0.85f) {
                            scaleDown = (mainLength * 0.85f) / totalWidth;
                        }

                        float ledSize = ledBaseSize * scaleDown;
                        float currentSpacing = spacing * scaleDown;
                        float adjustedTotalWidth = (wall.maxHits * ledSize) + ((wall.maxHits - 1) * currentSpacing);

                        float startX = vertical ? 0.0f : (-adjustedTotalWidth / 2.0f + ledSize / 2.0f);
                        float startY = vertical ? (-adjustedTotalWidth / 2.0f + ledSize / 2.0f) : 0.0f;

                        sf::Transform t;
                        t.translate({pos.x * physics.SCALE, pos.y * physics.SCALE});
                        t.rotate(sf::degrees(wall.body->GetAngle() * 180.0f / 3.14159f));

                        for (int k = 0; k < wall.maxHits; k++) {
                            sf::RectangleShape led({ledSize, ledSize});
                            led.setOrigin({ledSize / 2.0f, ledSize / 2.0f});
                            float lx = vertical ? startX : (startX + k * (ledSize + currentSpacing));
                            float ly = vertical ? (startY + k * (ledSize + currentSpacing)) : startY;

                            led.setPosition(t.transformPoint({lx, ly}));
                            led.setRotation(sf::degrees(wall.body->GetAngle() * 180.0f / 3.14159f));

                            if (k < wall.currentHits) {
                                led.setFillColor(sf::Color(100, 255, 100, 220));
                            } else {
                                led.setFillColor(sf::Color(255, 50, 50, 100));
                            }
                            gameBuffer.draw(led);
                        }
                    }
                }
            } // FIN BUCLE DE PAREDES
        } // FIN BUCLE Z-INDEX

        // ==============================================
        // --- DRAW PARTÍCULAS (SIEMPRE ARRIBA) ---
        // ==============================================
        const auto& particles = physics.getParticles();
        if (!particles.empty()) {
            sf::VertexArray va(sf::PrimitiveType::Triangles, particles.size() * 6);
            float pSize = (RENDER_WIDTH / 1080.0f) * 4.0f;

            for (size_t i = 0; i < particles.size(); ++i) {
                const auto& p = particles[i];
                sf::Color c = p.color;
                c.a = (std::uint8_t)(255.0f * (p.life / p.maxLife));

                sf::Vector2f tl = p.position + sf::Vector2f(-pSize, -pSize);
                sf::Vector2f tr = p.position + sf::Vector2f(pSize, -pSize);
                sf::Vector2f br = p.position + sf::Vector2f(pSize, pSize);
                sf::Vector2f bl = p.position + sf::Vector2f(-pSize, pSize);

                va[i*6 + 0] = sf::Vertex{tl, c};
                va[i*6 + 1] = sf::Vertex{tr, c};
                va[i*6 + 2] = sf::Vertex{br, c};
                va[i*6 + 3] = sf::Vertex{br, c};
                va[i*6 + 4] = sf::Vertex{bl, c};
                va[i*6 + 5] = sf::Vertex{tl, c};
            }
            gameBuffer.draw(va);
        }

        // ==============================================
        // --- DIBUJAR GIZMOS (UI DEL EDITOR, CAPA ABSOLUTA) ---
        // ==============================================
        if (selectedType == EntityType::Wall && selectedIndex >= 0 && selectedIndex < customWalls.size()) {
            const CustomWall& w = customWalls[selectedIndex];
            b2Vec2 pos = w.body->GetPosition();
            float rot = w.body->GetAngle();
            float wPx = w.width * physics.SCALE;
            float hPx = w.height * physics.SCALE;

            sf::RectangleShape bbox({wPx, hPx});
            bbox.setOrigin({wPx / 2.0f, hPx / 2.0f});
            bbox.setPosition({pos.x * physics.SCALE, pos.y * physics.SCALE});
            bbox.setRotation(sf::degrees(rot * 180.0f / 3.14159f));
            bbox.setFillColor(sf::Color(255, 255, 255, 10));
            bbox.setOutlineColor(sf::Color(255, 255, 255, 150));
            bbox.setOutlineThickness(1.5f);
            gameBuffer.draw(bbox);

            sf::Transform t;
            t.translate({pos.x * physics.SCALE, pos.y * physics.SCALE});
            t.rotate(sf::degrees(rot * 180.0f / 3.14159f));

            bool rotActive = (currentGizmo == GizmoState::Rotating) || (currentGizmo == GizmoState::None && isHoveringRotate);
            sf::Vector2f topEdgePx = t.transformPoint({0.0f, -hPx / 2.0f});
            sf::Vector2f rotHandlePx = t.transformPoint({0.0f, -hPx / 2.0f - 1.5f * physics.SCALE});

            sf::VertexArray lineToRot(sf::PrimitiveType::Lines, 2);
            lineToRot[0] = sf::Vertex{topEdgePx, sf::Color(255, 150, 0)};
            lineToRot[1] = sf::Vertex{rotHandlePx, sf::Color(255, 150, 0)};
            gameBuffer.draw(lineToRot);

            float rotRadius = (rotActive ? 0.4f : 0.3f) * physics.SCALE;
            sf::CircleShape rotCircle(rotRadius);
            rotCircle.setOrigin({rotRadius, rotRadius});
            rotCircle.setPosition(rotHandlePx);
            rotCircle.setFillColor(rotActive ? sf::Color(255, 150, 0, 180) : sf::Color(255, 150, 0, 100));
            rotCircle.setOutlineColor(sf::Color(255, 150, 0));
            rotCircle.setOutlineThickness(1.5f);
            gameBuffer.draw(rotCircle);

            sf::Vector2f cornersPx[4] = {
                    t.transformPoint({-wPx / 2.0f, -hPx / 2.0f}),
                    t.transformPoint({ wPx / 2.0f, -hPx / 2.0f}),
                    t.transformPoint({ wPx / 2.0f,  hPx / 2.0f}),
                    t.transformPoint({-wPx / 2.0f,  hPx / 2.0f})
            };

            for (int i = 0; i < 4; i++) {
                bool isHovered = (currentGizmo == GizmoState::Scaling && activeScaleCorner == i) || (currentGizmo == GizmoState::None && hoveredScaleCorner == i);
                float scaleSize = (isHovered ? 0.4f : 0.25f) * physics.SCALE;
                sf::RectangleShape scaleRect({scaleSize, scaleSize});
                scaleRect.setOrigin({scaleSize / 2.0f, scaleSize / 2.0f});
                scaleRect.setPosition(cornersPx[i]);
                scaleRect.setRotation(sf::degrees(rot * 180.0f / 3.14159f));
                scaleRect.setFillColor(isHovered ? sf::Color(0, 255, 100, 200) : sf::Color(0, 255, 100, 100));
                scaleRect.setOutlineColor(sf::Color(0, 255, 100));
                scaleRect.setOutlineThickness(1.5f);
                gameBuffer.draw(scaleRect);
            }
        }
        else if (selectedType == EntityType::WinZone) {
            float wPx = physics.winZoneSize[0] * physics.SCALE;
            float hPx = physics.winZoneSize[1] * physics.SCALE;
            float xPx = physics.winZonePos[0] * physics.SCALE;
            float yPx = physics.winZonePos[1] * physics.SCALE;

            sf::RectangleShape bbox({wPx, hPx});
            bbox.setOrigin({wPx / 2.0f, hPx / 2.0f});
            bbox.setPosition({xPx, yPx});
            bbox.setFillColor(sf::Color(255, 255, 255, 10));
            bbox.setOutlineColor(sf::Color(255, 255, 255, 150));
            bbox.setOutlineThickness(1.5f);
            gameBuffer.draw(bbox);

            sf::Vector2f cornersPx[4] = {
                sf::Vector2f(xPx - wPx / 2.0f, yPx - hPx / 2.0f),
                sf::Vector2f(xPx + wPx / 2.0f, yPx - hPx / 2.0f),
                sf::Vector2f(xPx + wPx / 2.0f, yPx + hPx / 2.0f),
                sf::Vector2f(xPx - wPx / 2.0f, yPx + hPx / 2.0f)
            };

            for (int i = 0; i < 4; i++) {
                bool isHovered = (currentGizmo == GizmoState::Scaling && activeScaleCorner == i) || (currentGizmo == GizmoState::None && hoveredScaleCorner == i);
                float scaleSize = (isHovered ? 0.4f : 0.25f) * physics.SCALE;
                sf::RectangleShape scaleRect({scaleSize, scaleSize});
                scaleRect.setOrigin({scaleSize / 2.0f, scaleSize / 2.0f});
                scaleRect.setPosition(cornersPx[i]);
                scaleRect.setFillColor(isHovered ? sf::Color(0, 255, 100, 200) : sf::Color(0, 255, 100, 100));
                scaleRect.setOutlineColor(sf::Color(0, 255, 100));
                scaleRect.setOutlineThickness(1.5f);
                gameBuffer.draw(scaleRect);
            }
        }
        else if (selectedType == EntityType::Racers && selectedIndex >= 0 && selectedIndex < bodies.size()) {
            b2Body* b = bodies[selectedIndex];
            float rSize = physics.currentRacerSize * physics.SCALE + (0.3f * physics.SCALE);

            sf::RectangleShape bbox({rSize, rSize});
            bbox.setOrigin({rSize / 2.0f, rSize / 2.0f});
            bbox.setPosition({b->GetPosition().x * physics.SCALE, b->GetPosition().y * physics.SCALE});
            bbox.setRotation(sf::degrees(b->GetAngle() * 180.0f / 3.14159f));
            bbox.setFillColor(sf::Color::Transparent);
            bbox.setOutlineColor(sf::Color::White);
            bbox.setOutlineThickness(2.0f);
            gameBuffer.draw(bbox);
        }

        gameBuffer.display();

        // 1. Declaramos un puntero a la textura final para no instanciar el sprite vacío (prohibido en SFML 3)
        const sf::Texture* textureToRender = nullptr;

        if (enableBloom) {
            // 1. EXTRAER BRILLO
            brightnessShader.setUniform("source", gameBuffer.getTexture());
            brightnessShader.setUniform("threshold", bloomThreshold);
            brightnessBuffer.clear(sf::Color::Black);
            sf::Sprite brightSprite(gameBuffer.getTexture());
            brightSprite.setScale({0.5f, 0.5f});
            brightnessBuffer.draw(brightSprite, &brightnessShader);
            brightnessBuffer.display();

            // 2. DESENFOQUE GAUSSIANO
            const sf::Texture* currentSource = &brightnessBuffer.getTexture();

            for (int i = 0; i < blurIterations; ++i) {
                // Pasada Horizontal
                blurShader.setUniform("source", *currentSource);
                blurShader.setUniform("dir", sf::Vector2f(1.0f / BLOOM_W, 0.0f));
                blurBuffer1.clear(sf::Color::Transparent);
                blurBuffer1.draw(sf::Sprite(*currentSource), &blurShader);
                blurBuffer1.display();

                // Pasada Vertical
                blurShader.setUniform("source", blurBuffer1.getTexture());
                blurShader.setUniform("dir", sf::Vector2f(0.0f, 1.0f / BLOOM_H));
                blurBuffer2.clear(sf::Color::Transparent);
                blurBuffer2.draw(sf::Sprite(blurBuffer1.getTexture()), &blurShader);
                blurBuffer2.display();

                currentSource = &blurBuffer2.getTexture();
            }

            // 3. FUSIÓN ADITIVA
            blendShader.setUniform("baseTexture", gameBuffer.getTexture());
            blendShader.setUniform("bloomTexture", *currentSource);
            blendShader.setUniform("multiplier", bloomMultiplier);

            finalBuffer.clear();
            sf::Sprite finalBaseSprite(gameBuffer.getTexture());
            finalBuffer.draw(finalBaseSprite, &blendShader);
            finalBuffer.display();

            textureToRender = &finalBuffer.getTexture();
        } else {
            textureToRender = &gameBuffer.getTexture();
        }

        if (recorder && recorder->isRecording) recorder->addFrame(*textureToRender);

        // Instanciamos el sprite directamente con la textura (RAII puro)
        sf::Sprite renderSprite(*textureToRender);

        window.clear(sf::Color(20, 20, 20));

        // 2. Ahora el resto del código que ya tenías para centrar el viewport
        // se aplica sobre el renderSprite que ya tiene su textura correcta.
        renderSprite.setScale({scaleBase, scaleBase});
        // offsetX y offsetY ya los calculamos arriba con el layout dinámico
        renderSprite.setPosition({offsetX, offsetY});

        // Opcional: Le metemos un marquito sutil al viewport para que se despegue del fondo
   /*   sf::RectangleShape viewportBorder(sf::Vector2f(DISPLAY_SIZE + 2, DISPLAY_SIZE + 2));
        viewportBorder.setPosition(offsetX - 1, offsetY - 1);
        viewportBorder.setFillColor(sf::Color::Transparent);
        viewportBorder.setOutlineColor(sf::Color(60, 60, 60));
        viewportBorder.setOutlineThickness(1.0f);

        window.draw(viewportBorder); */
        window.draw(renderSprite);

        ImGui::SFML::Render(window);
        window.display();
    }

    ImGui::SFML::Shutdown();
    return 0;
}