#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <SFML/Audio.hpp>
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <thread>
#include <unordered_map>
#include <vector>


namespace Category {
    enum Type {
        None = 0,
        Scene = 1 << 0,
        PlayerAircraft = 1 << 1,
        AlliedAircraft = 1 << 2,
        EnemyAircraft = 1 << 3,
    };
}

template<typename T>
concept LoadableFromFile = requires(T t, const std::filesystem::path & path) {
    { t.loadFromFile(path) } -> std::convertible_to<bool>;
};

template <LoadableFromFile T>
class ResourceManager {
public:
    void load(const std::filesystem::path& path) {
        std::unique_ptr<T> resource = std::make_unique<T>();
        if (!resource->loadFromFile(path)) {
            throw std::runtime_error("Can't load resource from file: " + path.string());
        }
        resources_[path] = std::move(resource);
    }

    T& get(const std::filesystem::path& path) const {
        auto it = resources_.find(path);
        assert(it != resources_.end() && "Resource not found.");
        return *(it->second);
    }

private:
    mutable std::unordered_map<std::filesystem::path, std::unique_ptr<T>> resources_;
};

class SceneNode;

struct Command {
    std::function<void(SceneNode&, sf::Time)> action_;
    unsigned int category_ = Category::None;
};

class SceneNode : public sf::Transformable, public sf::Drawable {
public:
    using Ptr = std::unique_ptr<SceneNode>;

    SceneNode() : parent_(nullptr) {}

    void addChild(Ptr child) {
        child->parent_ = this;
        children_.emplace_back(std::move(child));
    }

    Ptr detachChild(const SceneNode& node) {
        auto it = std::ranges::find_if(children_,
            [&](const Ptr& child) {
                return child.get() == &node;
            });

        assert(it != children_.end() && "Child not found");

        Ptr result = std::move(*it);
        result->parent_ = nullptr;
        children_.erase(it);
        return result;
    }

    void update(const sf::Time& dt) {
        updateCurrent(dt);
        updateChildren(dt);
    }

    void onCommand(const Command& command, const sf::Time& dt) {
        if (command.category_ & getCategory()) {
            command.action_(*this, dt);
        }

        for (auto& child : children_) {
            child->onCommand(command, dt);
        }
    }

    virtual unsigned int getCategory() const {
        return Category::Scene;
    }

private:
    void updateChildren(const sf::Time& dt) {
        for (auto& child : children_) {
            child->update(dt);
        }
    }

    virtual void updateCurrent(const sf::Time&) {}

    virtual void drawCurrent(sf::RenderTarget&, sf::RenderStates) const {}

    void draw(sf::RenderTarget& target, sf::RenderStates states) const final override {
        states.transform *= getTransform();
        drawCurrent(target, states);
        for (const auto& child : children_) {
            child->draw(target, states);
        }
    }

    SceneNode* parent_;
    std::vector<Ptr> children_;
};

class SpriteNode : public SceneNode {
public:
    explicit SpriteNode(const sf::Texture& texture)
        : sprite_(texture) {
    }

    SpriteNode(const sf::Texture& texture, const sf::IntRect& rect)
        : sprite_(texture, rect) {
    }

private:
    void drawCurrent(sf::RenderTarget& target, sf::RenderStates states) const override {
        target.draw(sprite_, states);
    }

    sf::Sprite sprite_;
};

using TextureHolder = ResourceManager<sf::Texture>;

class Entity : public SceneNode {
public:
    void setVelocity(const sf::Vector2f& velocity) {
        velocity_ = velocity;
    }

    void setVelocity(float dx, float dy) {
        velocity_ = sf::Vector2f(dx, dy);
    }

    void accelerate(float x, float y) {
        sf::Vector2f newVelocity(x, y);
        setVelocity(getVelocity() + newVelocity);
    }

    const sf::Vector2f& getVelocity() const {
        return velocity_;
    }

protected:
    void updateCurrent(const sf::Time& dt) override {
        move(velocity_ * dt.asSeconds());
    }

private:
    sf::Vector2f velocity_;
};

class Aircraft : public Entity {
public:
    enum Type {
        Eagle,
        Raptor
    };

    Aircraft(Type type, const std::filesystem::path& path, const TextureHolder& resources)
        : sprite_(resources.get(path)), type_(type) {
    }

    unsigned int getCategory() const override {
        // Only the main Eagle responds to player commands
        if (type_ == Eagle) {
            return Category::PlayerAircraft;
        }
        return Category::AlliedAircraft; // Escort aircraft don't respond to player input
    }

    Type getAircraftType() const {
        return type_;
    }

private:
    void drawCurrent(sf::RenderTarget& target, sf::RenderStates states) const override {
        target.draw(sprite_, states);
    }

    sf::Sprite sprite_;
    Type type_;
};

class CommandQueue {
public:
    void emplace(Command command) {
        cmd_.emplace(std::move(command));
    }

    bool isEmpty() const {
        return cmd_.empty();
    }

    Command pop() {
        Command result = std::move(cmd_.front());
        cmd_.pop();
        return result;
    }

private:
    std::queue<Command> cmd_;
};

class Player {
public:
    Player() {
        constexpr float playerSpeed = 200.f;

        keys_[sf::Keyboard::Key::Left] = "MoveLeft";
        keys_[sf::Keyboard::Key::Right] = "MoveRight";
        keys_[sf::Keyboard::Key::Up] = "MoveUp";
        keys_[sf::Keyboard::Key::Down] = "MoveDown";

        commands_["MoveUp"].action_ = [playerSpeed](SceneNode& node, sf::Time dt) {
            node.move({ 0.f, -playerSpeed * dt.asSeconds() });
            };

        commands_["MoveDown"].action_ = [playerSpeed](SceneNode& node, sf::Time dt) {
            node.move({ 0.f, playerSpeed * dt.asSeconds() });
            };

        commands_["MoveRight"].action_ = [playerSpeed](SceneNode& node, sf::Time dt) {
            node.move({ playerSpeed * dt.asSeconds(), 0.f });
            };

        commands_["MoveLeft"].action_ = [playerSpeed](SceneNode& node, sf::Time dt) {
            node.move({ -playerSpeed * dt.asSeconds(), 0.f });
            };

        for (auto& it : commands_) {
            it.second.category_ = Category::PlayerAircraft;
        }
    }

    void addKeys(const std::string& id, const sf::Keyboard::Key key) {
        keys_[key] = id;
    }

    void assignCommand(const std::string& id, const Command& c) {
        commands_[id] = c;
    }

    sf::Keyboard::Key getAssignKey(const std::string& action) {
        for (const auto& [key, id] : keys_) {
            if (id == action) return key;
        }
        return sf::Keyboard::Key::Unknown;
    }

    void handleEvent(const sf::Event& event, CommandQueue& commands) {
        if (const auto* keyPressed = event.getIf<sf::Event::KeyPressed>()) {
            if (keyPressed->scancode == sf::Keyboard::Scancode::P) {
                Command output;
                output.category_ = Category::PlayerAircraft;
                output.action_ = [](SceneNode& s, sf::Time) {
                    std::cout << s.getPosition().x << ","
                        << s.getPosition().y << "\n";
                    };
                commands.emplace(std::move(output));
            }
        }
    }

    static bool isRealtimeAction(const std::string& id) {
        return (id == "MoveUp" || id == "MoveDown" || id == "MoveRight" || id == "MoveLeft");
    }

    void handleRealtimeInput(CommandQueue& c) {
        for (const auto& [key, id] : keys_) {
            if (sf::Keyboard::isKeyPressed(key) && isRealtimeAction(id)) {
                c.emplace(commands_[id]);
            }
        }
    }

private:
    std::unordered_map<sf::Keyboard::Key, std::string> keys_;
    std::unordered_map<std::string, Command> commands_;
};

class World {
public:

    World(sf::RenderWindow& window)
        : window_(window),
        sceneView_(window_.getDefaultView()),
        worldBounds_({ 0.f, 0.f }, { sceneView_.getSize().x, 2000.f }),
        spawnPosition_(worldBounds_.size.x / 2.f, worldBounds_.size.y - sceneView_.getSize().y / 2.f),
        playerAircraft_(nullptr),
        scrollSpeed_(50.f) {
        loadTextures();
        buildScene();
        sceneView_.setCenter(spawnPosition_);
    }

    void update(sf::Time dt) {
        // Handle commands first
        while (!commandQueue_.isEmpty()) {
            sceneGraph_.onCommand(commandQueue_.pop(), dt);
        }

        // Normalize diagonal movement
        sf::Vector2f velocity = playerAircraft_->getVelocity();
        if (velocity.x != 0.f && velocity.y != 0.f)
            playerAircraft_->setVelocity(velocity / std::sqrt(2.f));
        playerAircraft_->accelerate(0.f, 0.f);

        // Update scene
        sceneGraph_.update(dt);

        // Keep player aircraft within bounds using clamp
        sf::FloatRect viewBounds(
            sceneView_.getCenter() - sceneView_.getSize() / 2.f,
            sceneView_.getSize());

        const float borderDistance = 40.f;
        sf::Vector2f position = playerAircraft_->getPosition();

        position.x = std::clamp(
            position.x,
            viewBounds.position.x + borderDistance,
            viewBounds.position.x + viewBounds.size.x - borderDistance
        );
        position.y = std::clamp(
            position.y,
            viewBounds.position.y + borderDistance,
            viewBounds.position.y + viewBounds.size.y - borderDistance
        );

        playerAircraft_->setPosition(position);

        // Update camera to follow player
        sceneView_.setCenter(playerAircraft_->getPosition());
    }

    void draw() {
        window_.setView(sceneView_);
        window_.draw(sceneGraph_);
    }

    CommandQueue& getCommandQueue() {
        return commandQueue_;
    }

private:
    enum Layer {
        Background,
        Air,
        LayerCount
    };

    void loadTextures() {
        textureHolder_.load("Textures/Space.png");
        textureHolder_.load("Textures/Eagle.png");
        textureHolder_.load("Textures/Raptor.png");
    }

    void buildScene() {
        for (size_t i = 0; i < LayerCount; ++i) {
            SceneNode::Ptr layer = std::make_unique<SceneNode>();
            sceneLayers_[i] = layer.get();
            sceneGraph_.addChild(std::move(layer));
        }

        sf::Texture& backgroundTex = textureHolder_.get("Textures/Space.png");
        backgroundTex.setRepeated(true);

        sf::IntRect backgroundRect(
            { 0, 0 },
            { static_cast<int>(worldBounds_.size.x), static_cast<int>(worldBounds_.size.y) }
        );

        auto background = std::make_unique<SpriteNode>(backgroundTex, backgroundRect);
        background->setPosition(worldBounds_.position);
        sceneLayers_[Background]->addChild(std::move(background));

        auto leader = std::make_unique<Aircraft>(Aircraft::Eagle, "Textures/Eagle.png", textureHolder_);
        playerAircraft_ = leader.get();
        playerAircraft_->setPosition(spawnPosition_);
        playerAircraft_->setVelocity(0.f, scrollSpeed_);
        sceneLayers_[Air]->addChild(std::move(leader));

        auto leftEscort = std::make_unique<Aircraft>(Aircraft::Raptor, "Textures/Raptor.png", textureHolder_);
        leftEscort->setPosition({ -80.f, 50.f });
        playerAircraft_->addChild(std::move(leftEscort));

        auto rightEscort = std::make_unique<Aircraft>(Aircraft::Raptor, "Textures/Raptor.png", textureHolder_);
        rightEscort->setPosition({ 80.f, 50.f });
        playerAircraft_->addChild(std::move(rightEscort));
    }

    sf::RenderWindow& window_;
    sf::View sceneView_;
    sf::FloatRect worldBounds_;
    sf::Vector2f spawnPosition_;
    Aircraft* playerAircraft_;
    float scrollSpeed_;
    CommandQueue commandQueue_;
    std::array<SceneNode*, LayerCount> sceneLayers_;
    SceneNode sceneGraph_;
    TextureHolder textureHolder_;
};

class Game {
public:
    Game()
        : window_(sf::VideoMode({ 1920u, 1080u }), "SFML Game"),
        world_(window_) {
    }

    void run() {
        sf::Clock clock;
        while (window_.isOpen()) {
            processInput();
            sf::Time dt = clock.restart();
            update(dt);
            render();
        }
    }

private:
    void processInput() {
        CommandQueue& commands = world_.getCommandQueue();

        while (const std::optional<sf::Event> event = window_.pollEvent()) {
            player_.handleEvent(*event, commands);
            if (event->is<sf::Event::Closed>()) {
                window_.close();
            }
        }
        player_.handleRealtimeInput(commands);
    }

    void update(sf::Time dt) {
        world_.update(dt);
    }

    void render() {
        window_.clear();
        world_.draw();
        window_.setView(window_.getDefaultView());
        window_.display();
    }

    sf::RenderWindow window_;
    Player player_;
    World world_;
};

class FontHolder {
public:
    void openFile(const std::string& id, const std::filesystem::path& path) {
        std::unique_ptr<sf::Font> u = std::make_unique<sf::Font>();
        if (!u->openFromFile(path)) {
            throw std::runtime_error("Font failed to load: " + path.string());
        }
        fonts_[id] = std::move(u);
    }

    const sf::Font& getFont(const std::string& id) const {
        auto it = fonts_.find(id);
        assert(it != fonts_.end() && "Font not found");
        return *(it->second);
    }

private:
    std::unordered_map<std::string, std::unique_ptr<sf::Font>> fonts_;
};

namespace States {
    enum ID {
        Title,
        Menu,
        Game,
        Pause,
        Loading
    };
}

class State {
public:
    struct Context {
        Context(sf::RenderWindow& window,
            TextureHolder& textures,
            FontHolder& fontHolder,
            Player& player)
            : window_(&window),
            textures_(&textures),
            fontHolder_(&fontHolder),
            player_(&player) {

            textures_->load("Textures/Menu.png");
            fontHolder_->openFile("RobotoMono-Italic-VariableFont_wght", "Fonts/RobotoMono-Italic-VariableFont_wght.ttf");
        }

        sf::RenderWindow* window_;
        TextureHolder* textures_;
        FontHolder* fontHolder_;
        Player* player_;
    };

    using Ptr = std::unique_ptr<State>;

    class StateStack {
    public:
        enum Action {
            Push,
            Pop,
            Clear,
        };

        explicit StateStack(State::Context context) : context_(context) {}

        void pushState(States::ID id) {
            pendingList_.push_back({ Push, id });
        }

        void popState() {
            pendingList_.push_back({ Pop, States::ID{} });
        }

        void clearState() {
            pendingList_.push_back({ Clear, States::ID{} });
        }

        template <typename T>
        void registerState(States::ID id) {
            factories_[id] = [this]() {
                return std::make_unique<T>(*this, context_);
                };
        }

        Ptr createState(States::ID id) {
            auto it = factories_.find(id);
            if (it != factories_.end()) {
                return it->second();
            }
            return nullptr;
        }

        void applyPendingChanges() {
            for (const auto& change : pendingList_) {
                switch (change.action) {
                case Push:
                    stack_.push_back(createState(change.id));
                    break;
                case Pop:
                    if (!stack_.empty()) {
                        stack_.pop_back();
                    }
                    break;
                case Clear:
                    stack_.clear();
                    break;
                }
            }
            pendingList_.clear();
        }

        void handleEvent(const sf::Event& event) {
            for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
                if (!(*it)->handleEvent(event)) {
                    break;
                }
            }
            applyPendingChanges();
        }

        void update(sf::Time dt) {
            for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
                if (!(*it)->update(dt)) {
                    break;
                }
            }
            applyPendingChanges();
        }

        void draw() {
            for (const auto& state : stack_) {
                state->draw();
            }
        }

        bool isEmpty() const {
            return stack_.empty();
        }

    private:
        struct PendingChange {
            Action action;
            States::ID id;
        };

        std::vector<Ptr> stack_;
        std::vector<PendingChange> pendingList_;
        Context context_;
        std::unordered_map<States::ID, std::function<Ptr()>> factories_;
    };

public:

    State(StateStack& stack, const Context& context)
        : stack_(&stack), context_(context) {
    }

    void requestPushState(States::ID id) {
        stack_->pushState(id);
    }

    void centerOrigin(sf::Text& text) {
        sf::FloatRect bounds = text.getGlobalBounds();
        text.setOrigin({ bounds.size.x / 2.0f, bounds.size.y / 2.0f });
    }

    void requestPopState() {
        stack_->popState();
    }

    void requestClearStates() {
        stack_->clearState();
    }

    Context& getContext() {
        return context_;
    }

    virtual ~State() = default;
    virtual void draw() = 0;
    virtual bool update(sf::Time dt) = 0;
    virtual bool handleEvent(const sf::Event& event) = 0;

private:
    StateStack* stack_;
    Context context_;
};

class TitleState : public State {
public:
    TitleState(State::StateStack& stack, State::Context context)
        : State(stack, context),
        mBackgroundSprite(context.textures_->get("Textures/Menu.png")),
        mText(context.fontHolder_->getFont("RobotoMono-Italic-VariableFont_wght")),
        mShowText(true), mTextEffectTime(sf::Time::Zero) {

        mText.setString("Press any key to continue");
        mText.setFillColor(sf::Color::White);

        // Center the text on screen
        sf::FloatRect textBounds = mText.getGlobalBounds();
        mText.setOrigin({ textBounds.size.x / 2.0f, textBounds.size.y / 2.0f });
        mText.setPosition({ context.window_->getView().getSize().x / 2.0f,
                          context.window_->getView().getSize().y * 0.8f });
    }

    virtual void draw() override {
        sf::RenderWindow& window = *getContext().window_;
        window.draw(mBackgroundSprite);
        if (mShowText) {
            window.draw(mText);
        }
    }

    virtual bool update(sf::Time dt) override {
        mTextEffectTime += dt;
        if (mTextEffectTime >= sf::seconds(0.5f)) {
            mShowText = !mShowText;
            mTextEffectTime = sf::Time::Zero;
        }
        return true;
    }

    virtual bool handleEvent(const sf::Event& event) override {
        if (event.is<sf::Event::KeyPressed>()) {
            requestPopState();
            requestPushState(States::Loading); 
        }
        return true;
    }


private:
    sf::Sprite mBackgroundSprite;
    sf::Text mText;
    bool mShowText;
    sf::Time mTextEffectTime;
};

class MenuState : public State {
public:
    MenuState(State::StateStack& stack, State::Context context) : State(stack, context) {
        mOptionIndex = 0;

        // Create Play option
        sf::Text playOption(context.fontHolder_->getFont("RobotoMono-Italic-VariableFont_wght"));
        playOption.setString("Play");
        centerOrigin(playOption);
        playOption.setPosition({ context.window_->getView().getSize().x / 2.f,
                               context.window_->getView().getSize().y / 2.f - 50.f });
        mOptions.push_back(playOption);

        // Create Exit option
        sf::Text exitOption(context.fontHolder_->getFont("RobotoMono-Italic-VariableFont_wght"));
        exitOption.setString("Exit");
        centerOrigin(exitOption);
        exitOption.setPosition({ context.window_->getView().getSize().x / 2.f,
                               context.window_->getView().getSize().y / 2.f + 50.f });
        mOptions.push_back(exitOption);

        updateOptionText();
    }

    virtual bool update(sf::Time dt) override {
        return false;
    }

    virtual void draw() override {
        sf::RenderWindow& window = *getContext().window_;
        for (const auto& option : mOptions) {
            window.draw(option);
        }
    }

    virtual bool handleEvent(const sf::Event& event) override {
        if (const auto* keyPressed = event.getIf<sf::Event::KeyPressed>()) {
            if (keyPressed->code == sf::Keyboard::Key::Up) {
                if (mOptionIndex > 0)
                    mOptionIndex--;
                else
                    mOptionIndex = mOptions.size() - 1;
                updateOptionText();
            }
            else if (keyPressed->code == sf::Keyboard::Key::Down) {
                if (mOptionIndex < mOptions.size() - 1)
                    mOptionIndex++;
                else
                    mOptionIndex = 0;
                updateOptionText();
            }
            else if (keyPressed->code == sf::Keyboard::Key::Enter) {
                if (mOptionIndex == Play) {
                    requestPopState();
                    requestPushState(States::Game);
                }
                else if (mOptionIndex == Exit) {
                    requestClearStates();
                }
            }
            return true;
        }
        return false;
    }

private:
    void updateOptionText() {
        if (mOptions.empty())
            return;

        for (auto& text : mOptions) {
            text.setFillColor(sf::Color::White);
        }
        mOptions[mOptionIndex].setFillColor(sf::Color::Red);
    }

    enum OptionNames {
        Play,
        Exit,
    };
    std::vector<sf::Text> mOptions;
    std::size_t mOptionIndex;
};

class GameState : public State {
public:
    GameState(State::StateStack& stack, State::Context context)
        : State(stack, context), world_(*context.window_), player_(*context.player_) {
    }

    virtual void draw() override {
        world_.draw();
    }

    virtual bool update(sf::Time dt) override {
        getContext().player_->handleRealtimeInput(world_.getCommandQueue()); 
        world_.update(dt);
        return true;
    }


    virtual bool handleEvent(const sf::Event& event) override {
        // Handle pause functionality
        if (const auto* keyPressed = event.getIf<sf::Event::KeyPressed>()) {
            if (keyPressed->code == sf::Keyboard::Key::Escape) {
                requestPushState(States::Pause);
                return false; // Don't pass event to player when pausing
            }
        }

        player_.handleEvent(event, world_.getCommandQueue());
        return true;
    }

private:
    World world_;
    Player& player_;
};

class PauseState : public State {
public:
    PauseState(State::StateStack& stack, State::Context context)
        : State(stack, context),
        mPausedText(context.fontHolder_->getFont("RobotoMono-Italic-VariableFont_wght")),
        mInstructionText(context.fontHolder_->getFont("RobotoMono-Italic-VariableFont_wght")) {

        // Setup paused text
        mPausedText.setString("Game Paused");
        mPausedText.setFillColor(sf::Color::White);
        mPausedText.setCharacterSize(50);
        sf::FloatRect pausedBounds = mPausedText.getGlobalBounds();
        mPausedText.setOrigin({ pausedBounds.size.x / 2.0f, pausedBounds.size.y / 2.0f });
        mPausedText.setPosition({ context.window_->getView().getSize().x / 2.0f,
                                context.window_->getView().getSize().y / 2.0f - 50.0f });

        // Setup instruction text
        mInstructionText.setString("Press Backspace to return to menu, Escape to resume");
        mInstructionText.setFillColor(sf::Color::White);
        mInstructionText.setCharacterSize(20);
        sf::FloatRect instructionBounds = mInstructionText.getGlobalBounds();
        mInstructionText.setOrigin({ instructionBounds.size.x / 2.0f, instructionBounds.size.y / 2.0f });
        mInstructionText.setPosition({ context.window_->getView().getSize().x / 2.0f,
                                     context.window_->getView().getSize().y / 2.0f + 50.0f });
    }

    virtual void draw() override {
        sf::RenderWindow& window = *getContext().window_;
        window.setView(window.getDefaultView());

        // Draw semi-transparent background
        sf::RectangleShape backgroundShape;
        backgroundShape.setFillColor(sf::Color(0, 0, 0, 150));
        backgroundShape.setSize(sf::Vector2f(window.getSize()));
        window.draw(backgroundShape);

        // Draw text
        window.draw(mPausedText);
        window.draw(mInstructionText);
    }

    virtual bool update(sf::Time dt) override {
        return false;
    }

    virtual bool handleEvent(const sf::Event& event) override {
        if (const auto* keyPressed = event.getIf<sf::Event::KeyPressed>()) {
            if (keyPressed->code == sf::Keyboard::Key::Backspace) {
                requestClearStates();
                requestPushState(States::Menu);
            }
            else if (keyPressed->code == sf::Keyboard::Key::Escape) {
                requestPopState(); 
            }
            return false; 
        }
        return false;
    }

private:
    sf::Text mPausedText;
    sf::Text mInstructionText;
};

class ParallelTask {
public:
    ParallelTask() : finished_(false) {}

    void execute() {
        clock_.restart();
        thread_ = std::jthread([this](std::stop_token stoken) {
            runTask(stoken);
            });
    }

    ~ParallelTask() {
        if (thread_.joinable()) {
            thread_.request_stop();
        }
    }

    float getCompletion() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return std::min(clock_.getElapsedTime().asSeconds() / 3.f, 1.0f); 
    }

    bool taskFinished() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return finished_.load();
    }

private:
    void runTask(std::stop_token stoken) {
        while (!stoken.stop_requested()) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (clock_.getElapsedTime().asSeconds() >= 3.f) {
                finished_ = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

private:
    std::jthread thread_;
    mutable std::mutex mtx_;
    std::atomic<bool> finished_;
    sf::Clock clock_;
};

class LoadingState : public State {
public:
    LoadingState(StateStack& stack, State::Context context)
        : State(stack, context),
        window_(*context.window_),
        loadingText_(context.fontHolder_->getFont("RobotoMono-Italic-VariableFont_wght")) {

        loadingText_.setString("Loading Resources...");
        centerOrigin(loadingText_);
        loadingText_.setPosition({
            static_cast<float>(window_.getSize().x) / 2.f,
            static_cast<float>(window_.getSize().y) / 2.f
            });

        progressBarBackground_.setFillColor(sf::Color::White);
        progressBarBackground_.setSize({ 400.f, 10.f });
        progressBarBackground_.setPosition({
            (window_.getSize().x - 400.f) / 2.f,
            loadingText_.getPosition().y + 50.f
            });

        progressBar_.setFillColor(sf::Color::Green);
        progressBar_.setSize({ 0.f, 10.f });
        progressBar_.setPosition(progressBarBackground_.getPosition());

        setCompletion(0.f);
        loadingTask_.execute(); 
    }

    virtual void draw() override {
        window_.clear(sf::Color::Black);
        window_.draw(loadingText_);
        window_.draw(progressBarBackground_);
        window_.draw(progressBar_);
    }

    virtual bool update(sf::Time dt) override {
        if (loadingTask_.taskFinished()) {
            requestPopState();
            requestPushState(States::Menu);
        }
        else {
            setCompletion(loadingTask_.getCompletion());
        }
        return false;
    }

    virtual bool handleEvent(const sf::Event& event) override {
        return false; 
    }

private:
    void setCompletion(float percent) {
        if (percent > 1.0f) percent = 1.0f;
        progressBar_.setSize({ progressBarBackground_.getSize().x * percent, 10.f });
    }

    sf::RenderWindow& window_;
    sf::Text loadingText_;
    sf::RectangleShape progressBarBackground_;
    sf::RectangleShape progressBar_;
    ParallelTask loadingTask_;
};


class Application {
public:
    Application(sf::RenderWindow& window, const State::Context& context)
        : window_(window), stateStack_(context) {
        registerStates();
        stateStack_.pushState(States::Title);
    }

    void processEvents() {
        while (std::optional<sf::Event> event = window_.pollEvent()) {
            stateStack_.handleEvent(*event);
        }
    }

    void update(sf::Time dt) {
        stateStack_.update(dt);
    }

    void render() {
        window_.clear();
        stateStack_.draw();
        window_.display();
    }

    void run() {
        processEvents();
        render();
        if (stateStack_.isEmpty())
            window_.close();
    }

private:
    void registerStates() {
        stateStack_.registerState<TitleState>(States::Title);
        stateStack_.registerState<LoadingState>(States::Loading); 
        stateStack_.registerState<MenuState>(States::Menu);
        stateStack_.registerState<GameState>(States::Game);
        stateStack_.registerState<PauseState>(States::Pause);
    }


    sf::RenderWindow& window_;
    State::StateStack stateStack_;
};

class StatefulGame {
public:
    StatefulGame()
        : window_(sf::VideoMode({ 1920u, 1080u }), "SFML Game"),
        context_(window_, textureHolder_, fontHolder_, player_),
        app_(window_, context_) {
    }

    void run() {
        sf::Clock clock;
        while (window_.isOpen()) {
            sf::Time dt = clock.restart();
            app_.processEvents();
            app_.update(dt);
            app_.render();
        }
    }

private:
    sf::RenderWindow window_;
    TextureHolder textureHolder_;
    FontHolder fontHolder_;
    Player player_;
    State::Context context_;
    Application app_;
};

int main() {
    try {
        StatefulGame game;
        game.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}