#include "Halide.h"
#include <assert.h>
#include <stdio.h>
#include <algorithm>
#include <functional>
#include <map>
#include <numeric>

using std::map;
using std::vector;
using std::string;

using namespace Halide;
using namespace Halide::Internal;

typedef map<string, vector<string>> CallGraphs;

class CheckCalls : public IRVisitor {
public:
    CheckCalls(const map<string, Function> &funcs) : funcs(funcs) {}

    CallGraphs calls; // Caller -> vector of callees
private:
    map<string, Function> funcs;
    string producer = "";

    using IRVisitor::visit;

    void visit(const Producer *op) {
        assert(funcs.count(op->name));
        Stmt produce = op->body;
        Stmt update;

        if (!funcs[op->name].updates().empty()) {
            // Peel the let/if stmts until we find a block
            Stmt body = op->body;
            while (true) {
                const LetStmt *let = body.as<LetStmt>();
                const IfThenElse *if_else = body.as<IfThenElse>();
                if (let) {
                    body = let->body;
                } else if (if_else) {
                    body = if_else->then_case;
                } else {
                    break;
                }
            }
            if (const Block *block = body.as<Block>()) {
                produce = block->first;
                update = block->rest;
            }
        }

        string old_producer = producer;
        producer = op->name;
        calls[producer]; // Make sure each producer is allocated a slot
        produce.accept(this);
        producer = old_producer;

        if (update.defined()) {
            assert(!funcs[op->name].updates().empty());
            // Just lump all the update stages together
            producer = op->name + ".update(" + std::to_string(0) + ")";
            calls[producer]; // Make sure each producer is allocated a slot
            update.accept(this);
            producer = old_producer;
        }
        producer = old_producer;
    }

    void visit(const Load *op) {
        IRVisitor::visit(op);
        if (!producer.empty()) {
            assert(calls.count(producer) > 0);
            vector<string> &callees = calls[producer];
            if(std::find(callees.begin(), callees.end(), op->name) == callees.end()) {
                callees.push_back(op->name);
            }
        }
    }
};

int check_call_graphs(CallGraphs &result, CallGraphs &expected) {
    if (result.size() != expected.size()) {
        printf("Expect %d callers instead of %d\n", (int)expected.size(), (int)result.size());
        return -1;
    }
    for (auto &iter : expected) {
        if (result.count(iter.first) == 0) {
            printf("Expect %s to be in the call graphs\n", iter.first.c_str());
            return -1;
        }
        vector<string> &expected_callees = iter.second;
        vector<string> &result_callees = result[iter.first];
        std::sort(expected_callees.begin(), expected_callees.end());
        std::sort(result_callees.begin(), result_callees.end());
        if (expected_callees != result_callees) {
            string expected_str = std::accumulate(
                expected_callees.begin(), expected_callees.end(), std::string{},
                [](const string &a, const string &b) {
                    return a.empty() ? b : a + ", " + b;
                });
            string result_str = std::accumulate(
                result_callees.begin(), result_callees.end(), std::string{},
                [](const string &a, const string &b) {
                    return a.empty() ? b : a + ", " + b;
                });

            printf("Expect calless of %s to be (%s); got (%s) instead\n",
                    iter.first.c_str(), expected_str.c_str(), result_str.c_str());
            return -1;
        }

    }
    return 0;
}

int check_image(const Image<int> &im, const std::function<int(int,int)> &func) {
    for (int y = 0; y < im.height(); y++) {
        for (int x = 0; x < im.width(); x++) {
            int correct = func(x, y);
            if (im(x, y) != correct) {
                printf("im(%d, %d) = %d instead of %d\n",
                       x, y, im(x, y), correct);
                return -1;
            }
        }
    }
    return 0;
}

int calling_wrap_no_op_test() {
    Var x("x"), y("y");

    {
        ImageParam img(Int(32), 2);
        Func f("f");
        f(x, y) = img(x, y);

        // Calling wrap on the same ImageParam for the same Func multiple times should
        // return the same wrapper
        Func wrapper = img.in(f);
        for (int i = 0; i < 5; ++i) {
            Func temp = img.in(f);
            if (wrapper.name() != temp.name()) {
                std::cerr << "Expect " << wrapper.name() << "; got " << temp.name() << " instead\n";
                return -1;
            }
        }
    }

    {
        ImageParam img(Int(32), 2);
        Func f("f");
        f(x, y) = img(x, y);

        // Should return the same global wrapper
        Func wrapper1 = img.in();
        Func wrapper2 = img.in();
        if (wrapper1.name() != wrapper2.name()) {
            std::cerr << "Expect " << wrapper1.name() << "; got " << wrapper2.name() << " instead\n";
            return -1;
        }
    }

    {
        ImageParam img(Int(32), 2);
        Func e("e"), f("f"), g("g"), h("h");
        e(x, y) = img(x, y);
        f(x, y) = img(x, y);
        g(x, y) = img(x, y);
        h(x, y) = img(x, y);

        Func wrapper1 = img.in({e, f, g});
        Func wrapper2 = img.in({g, f, e});
        if (wrapper1.name() != wrapper2.name()) {
            std::cerr << "Expect " << wrapper1.name() << "; got " << wrapper2.name() << " instead\n";
            return -1;
        }
    }

    return 0;
}

int func_wrap_test() {
    Func source("source"), g("g");
    Var x("x"), y("y");

    source(x) = x;
    ImageParam img(Int(32), 1, "img");
    Image<int> buf = source.realize(200);
    img.set(buf);

    g(x, y) = img(x);

    Func wrapper = img.in(g).compute_root();
    Func img_f = img;
    img_f.compute_root();

    // Check the call graphs.
    // Expect 'g' to call 'wrapper', 'wrapper' to call 'img_f', 'img_f' to call 'img'
    Module m = g.compile_to_module({g.infer_arguments()});
    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {g.name(), g.function()},
        {wrapper.name(), wrapper.function()},
    };
    CheckCalls c(funcs);
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {g.name(), {wrapper.name()}},
        {wrapper.name(), {img_f.name()}},
        {img_f.name(), {img.name()}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }

    Image<int> im = g.realize(200, 200);
    auto func = [](int x, int y) { return x; };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int multiple_funcs_sharing_wrapper_test() {
    Func source("source"), g1("g1"), g2("g2"), g3("g3");
    Var x("x"), y("y");

    source(x) = x;
    ImageParam img(Int(32), 1, "img");
    Image<int> buf = source.realize(200);
    img.set(buf);

    g1(x, y) = img(x);
    g2(x, y) = img(x);
    g3(x, y) = img(x);

    Func im_wrapper = img.in({g1, g2, g3}).compute_root();
    Func img_f = img;
    img_f.compute_root();

    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {g1.name(), g1.function()},
        {g2.name(), g2.function()},
        {g3.name(), g3.function()},
        {im_wrapper.name(), im_wrapper.function()},
    };

    {
        // Check the call graphs.
        // Expect 'g1' to call 'im_wrapper', 'im_wrapper' to call 'img_f', 'img_f' to call 'img'
        Module m = g1.compile_to_module({g1.infer_arguments()});
        CheckCalls c(funcs);
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {g1.name(), {im_wrapper.name()}},
            {im_wrapper.name(), {img_f.name()}},
            {img_f.name(), {img.name()}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }

        Image<int> im = g1.realize(200, 200);
        auto func = [](int x, int y) { return x; };
        if (check_image(im, func)) {
            return -1;
        }
    }

    {
        // Check the call graphs.
        // Expect 'g2' to call 'im_wrapper', 'im_wrapper' to call 'img_f', 'f' to call 'img'
        Module m = g2.compile_to_module({g2.infer_arguments()});
        CheckCalls c(funcs);
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {g2.name(), {im_wrapper.name()}},
            {im_wrapper.name(), {img_f.name()}},
            {img_f.name(), {img.name()}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }

        Image<int> im = g2.realize(200, 200);
        auto func = [](int x, int y) { return x; };
        if (check_image(im, func)) {
            return -1;
        }
    }

    {
        // Check the call graphs.
        // Expect 'g3' to call 'im_wrapper', 'im_wrapper' to call 'img_f', 'f' to call 'img'
        Module m = g3.compile_to_module({g3.infer_arguments()});
        CheckCalls c(funcs);
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {g3.name(), {im_wrapper.name()}},
            {im_wrapper.name(), {img_f.name()}},
            {img_f.name(), {img.name()}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }

        Image<int> im = g3.realize(200, 200);
        auto func = [](int x, int y) { return x; };
        if (check_image(im, func)) {
            return -1;
        }
    }
    return 0;
}

int global_wrap_test() {
    Func source("source"), g("g"), h("h"), i("i");
    Var x("x"), y("y");

    source(x, y) = x + y;
    ImageParam img(Int(32), 2, "img");
    Image<int> buf = source.realize(200, 200);
    img.set(buf);

    g(x, y) = img(x, y);
    h(x, y) = g(x, y) + img(x, y);

    Var xi("xi"), yi("yi"), t("t");
    Func wrapper = img.in();
    Func img_f = img;
    img_f.compute_root();
    h.compute_root().tile(x, y, xi, yi, 16, 16).fuse(x, y, t).parallel(t);
    g.compute_at(h, yi);
    wrapper.compute_at(h, yi).tile(_0, _1, xi, yi, 8, 8).fuse(xi, yi, t).vectorize(t, 4);

    // Check the call graphs.
    // Expect 'g' to call 'wrapper', 'wrapper' to call 'img_f', 'img_f' to call 'img',
    // 'h' to call 'wrapper' and 'g'
    Module m = h.compile_to_module({h.infer_arguments()});
    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {g.name(), g.function()},
        {h.name(), h.function()},
        {wrapper.name(), wrapper.function()},
    };
    CheckCalls c(funcs);
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {h.name(), {g.name(), wrapper.name()}},
        {g.name(), {wrapper.name()}},
        {wrapper.name(), {img_f.name()}},
        {img_f.name(), {img.name()}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }

    Image<int> im = h.realize(200, 200);
    auto func = [](int x, int y) { return 2*(x + y); };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int update_defined_after_wrap_test() {
    Func source("source"), g("g");
    Var x("x"), y("y");

    source(x, y) = x + y;
    ImageParam img(Int(32), 2, "img");
    Image<int> buf = source.realize(200, 200);
    img.set(buf);

    g(x, y) = img(x, y);

    Func wrapper = img.in(g);

    // Update of 'g' is defined after img.in(g) is called. g's updates should
    // still call img's wrapper.
    RDom r(0, 100, 0, 100);
    r.where(r.x < r.y);
    g(r.x, r.y) += 2*img(r.x, r.y);

    Param<bool> param;

    Var xi("xi");
    RVar rxo("rxo"), rxi("rxi");
    g.specialize(param).vectorize(x, 8).unroll(x, 2).split(x, x, xi, 4).parallel(x);
    g.update(0).split(r.x, rxo, rxi, 2).unroll(rxi);
    Func img_f = img;
    img_f.compute_root();
    wrapper.compute_root().vectorize(_0, 8).unroll(_0, 2).split(_0, _0, xi, 4).parallel(_0);

    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {g.name(), g.function()},
        {wrapper.name(), wrapper.function()},
    };

    {
        param.set(true);

        // Check the call graphs.
        // Expect initialization of 'g' to call 'wrapper' and its update to call
        // 'wrapper' and 'g', wrapper' to call 'img_f', 'img_f' to call 'img'
        Module m = g.compile_to_module({g.infer_arguments()});
        CheckCalls c(funcs);
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {g.name(), {wrapper.name()}},
            {g.update(0).name(), {wrapper.name(), g.name()}},
            {wrapper.name(), {img_f.name()}},
            {img_f.name(), {img.name()}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }

        Image<int> im = g.realize(200, 200);
        auto func = [](int x, int y) {
            return ((0 <= x && x <= 99) && (0 <= y && y <= 99) && (x < y)) ? 3*(x + y) : (x + y);
        };
        if (check_image(im, func)) {
            return -1;
        }
    }

    {
        param.set(false);

        // Check the call graphs.
        // Expect initialization of 'g' to call 'wrapper' and its update to call
        // 'wrapper' and 'g', wrapper' to call 'img_f', 'img_f' to call 'img'
        Module m = g.compile_to_module({g.infer_arguments()});
        CheckCalls c(funcs);
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {g.name(), {wrapper.name()}},
            {g.update(0).name(), {wrapper.name(), g.name()}},
            {wrapper.name(), {img_f.name()}},
            {img_f.name(), {img.name()}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }

        Image<int> im = g.realize(200, 200);
        auto func = [](int x, int y) {
            return ((0 <= x && x <= 99) && (0 <= y && y <= 99) && (x < y)) ? 3*(x + y) : (x + y);
        };
        if (check_image(im, func)) {
            return -1;
        }
    }

    return 0;
}

int rdom_wrapper_test() {
    Func source("source"), g("g");
    Var x("x"), y("y");

    source(x, y) = x + y;
    ImageParam img(Int(32), 2, "img");
    Image<int> buf = source.realize(200, 200);
    img.set(buf);

    g(x, y) = 10;
    g(x, y) += 2 * img(x, x);
    g(x, y) += 3 * img(y, y);

    // Make a global wrapper on 'g', so that we can schedule initialization
    // and the update on the same compute level at the global wrapper
    Func wrapper = g.in().compute_root();
    g.compute_at(wrapper, x);
    Func img_f = img;
    img_f.compute_root();

    // Check the call graphs.
    // Expect 'wrapper' to call 'g', initialization of 'g' to call nothing
    // and its update to call 'img_f' and 'g', 'img_f' to call 'img'
    Module m = wrapper.compile_to_module({wrapper.infer_arguments()});
    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {g.name(), g.function()},
        {wrapper.name(), wrapper.function()},
    };
    CheckCalls c(funcs);
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {g.name(), {}},
        {g.update(0).name(), {img_f.name(), g.name()}},
        {wrapper.name(), {g.name()}},
        {img_f.name(), {img.name()}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }

    Image<int> im = wrapper.realize(200, 200);
    auto func = [](int x, int y) { return 4*x + 6* y + 10; };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int global_and_custom_wrap_test() {
    Func source("source"), g("g"), result("result");
    Var x("x"), y("y");

    source(x) = x;
    ImageParam img(Int(32), 1, "img");
    Image<int> buf = source.realize(200);
    img.set(buf);

    g(x, y) = img(x);
    result(x, y) = img(x) + g(x, y);

    Func img_in_g = img.in(g).compute_at(g, x);
    Func img_wrapper = img.in().compute_at(result, y);
    Func img_f = img;
    img_f.compute_root();
    g.compute_at(result, y);

    // Check the call graphs.
    // Expect 'result' to call 'g' and 'img_wrapper', 'g' to call 'img_in_g',
    // 'img_wrapper' to call 'f', img_in_g' to call 'img_f', 'f' to call 'img'
    Module m = result.compile_to_module({result.infer_arguments()});
    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {g.name(), g.function()},
        {result.name(), result.function()},
        {img_in_g.name(), img_in_g.function()},
        {img_wrapper.name(), img_wrapper.function()},
    };
    CheckCalls c(funcs);
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {result.name(), {g.name(), img_wrapper.name()}},
        {g.name(), {img_in_g.name()}},
        {img_wrapper.name(), {img_f.name()}},
        {img_in_g.name(), {img_f.name()}},
        {img_f.name(), {img.name()}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }

    Image<int> im = result.realize(200, 200);
    auto func = [](int x, int y) { return 2*x; };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}


int wrapper_depend_on_mutated_func_test() {
    Func source("sourceo"), f("f"), g("g"), h("h");
    Var x("x"), y("y");

    source(x, y) = x + y;
    ImageParam img(Int(32), 2, "img");
    Image<int> buf = source.realize(200, 200);
    img.set(buf);

    f(x, y) = img(x, y);
    g(x, y) = f(x, y);
    h(x, y) = g(x, y);

    Var xo("xo"), xi("xi");
    Func img_f = img;
    img_f.compute_root();
    f.compute_at(g, y).vectorize(x, 8);
    g.compute_root();
    Func img_in_f = img.in(f);
    Func g_in_h = g.in(h).compute_root();
    g_in_h.compute_at(h, y).vectorize(x, 8);
    img_in_f.compute_at(f, y).split(_0, xo, xi, 8);

    // Check the call graphs.
    // Expect 'h' to call 'g_in_h', 'g_in_h' to call 'g', 'g' to call 'f',
    // 'f' to call 'img_in_f', img_in_f' to call 'img_f', 'img_f' to call 'img'
    Module m = h.compile_to_module({h.infer_arguments()});
    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {f.name(), f.function()},
        {g.name(), g.function()},
        {h.name(), h.function()},
        {img_in_f.name(), img_in_f.function()},
        {g_in_h.name(), g_in_h.function()},
    };
    CheckCalls c(funcs);
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {h.name(), {g_in_h.name()}},
        {g_in_h.name(), {g.name()}},
        {g.name(), {f.name()}},
        {f.name(), {img_in_f.name()}},
        {img_in_f.name(), {img_f.name()}},
        {img_f.name(), {img.name()}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }

    Image<int> im = h.realize(200, 200);
    auto func = [](int x, int y) { return x + y; };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int wrapper_on_wrapper_test() {
    Func source("source"), g("g"), h("h");
    Var x("x"), y("y");

    source(x, y) = x + y;
    ImageParam img(Int(32), 2, "img");
    Image<int> buf = source.realize(200, 200);
    img.set(buf);

    g(x, y) = img(x, y) + img(x, y);
    Func img_in_g = img.in(g).compute_root();
    Func img_in_img_in_g = img.in(img_in_g).compute_root();
    h(x, y) = g(x, y) + img(x, y) + img_in_img_in_g(x, y);

    Func img_f = img;
    img_f.compute_root();
    g.compute_root();
    Func img_in_h = img.in(h).compute_root();
    Func g_in_h = g.in(h).compute_root();

    // Check the call graphs.
    Module m = h.compile_to_module({h.infer_arguments()});
    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {g.name(), g.function()},
        {h.name(), h.function()},
        {img_in_g.name(), img_in_g.function()},
        {img_in_img_in_g.name(), img_in_img_in_g.function()},
        {img_in_h.name(), img_in_h.function()},
        {g_in_h.name(), g_in_h.function()},
    };
    CheckCalls c(funcs);
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {h.name(), {img_in_h.name(), g_in_h.name(), img_in_img_in_g.name()}},
        {img_in_h.name(), {img_f.name()}},
        {g_in_h.name(), {g.name()}},
        {g.name(), {img_in_g.name()}},
        {img_in_g.name(), {img_in_img_in_g.name()}},
        {img_in_img_in_g.name(), {img_f.name()}},
        {img_f.name(), {img.name()}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }

    Image<int> im = h.realize(200, 200);
    auto func = [](int x, int y) { return 4*(x + y); };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int wrapper_on_rdom_predicate_test() {
    Func source("source"), g("g"), h("h");
    Var x("x"), y("y");

    source(x, y) = x + y;
    ImageParam img(Int(32), 2, "img");
    Image<int> buf = source.realize(200, 200);
    img.set(buf);

    g(x, y) = 10;
    h(x, y) = 5;

    RDom r(0, 100, 0, 100);
    r.where(img(r.x, r.y) + h(r.x, r.y) < 50);
    g(r.x, r.y) += h(r.x, r.y);

    Func h_wrapper = h.in().store_root().compute_at(g, r.y);
    Func img_in_g = img.in(g).compute_at(g, r.x);
    Func img_f = img;
    img_f.compute_root();
    h.compute_root();

    // Check the call graphs.
    // Expect 'g' to call nothing, update of 'g' to call 'g', img_in_g', and 'h_wrapper',
    // 'img_in_g' to call 'img_f', 'img_f' to call 'img', 'h_wrapper' to call 'h',
    // 'h' to call nothing
    Module m = g.compile_to_module({g.infer_arguments()});
    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {g.name(), g.function()},
        {h.name(), h.function()},
        {h_wrapper.name(), h_wrapper.function()},
        {img_in_g.name(), img_in_g.function()},
    };
    CheckCalls c(funcs);
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {g.name(), {}},
        {g.update(0).name(), {g.name(), img_in_g.name(), h_wrapper.name()}},
        {img_in_g.name(), {img_f.name()}},
        {img_f.name(), {img.name()}},
        {h_wrapper.name(), {h.name()}},
        {h.name(), {}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }

    Image<int> im = g.realize(200, 200);
    auto func = [](int x, int y) {
        return ((0 <= x && x <= 99) && (0 <= y && y <= 99) && (x + y + 5 < 50)) ? 15 : 10;
    };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int two_fold_wrapper_test() {
    Func source("source"), img_in_output_in_output, img_in_output, output("output");
    Var x("x"), y("y");

    source(x, y) = 2*x + 3*y;
    ImageParam img(Int(32), 2, "img");
    Image<int> buf = source.realize(1024, 1024);
    img.set(buf);

    Func img_f = img;
    img_f.compute_root();

    output(x, y) = img(y, x);

    Var xi("xi"), yi("yi");
    output.tile(x, y, xi, yi, 8, 8);

    img_in_output = img.in(output).compute_at(output, x).vectorize(_0).unroll(_1);
    img_in_output_in_output = img_in_output.in(output).compute_at(output, x).unroll(_0).unroll(_1);

    // Check the call graphs.
    Module m = output.compile_to_module({output.infer_arguments()});
    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {output.name(), output.function()},
        {img_in_output.name(), img_in_output.function()},
        {img_in_output_in_output.name(), img_in_output_in_output.function()},
    };
    CheckCalls c(funcs);
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {output.name(), {img_in_output_in_output.name()}},
        {img_in_output_in_output.name(), {img_in_output.name()}},
        {img_in_output.name(), {img_f.name()}},
        {img_f.name(), {img.name()}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }

    Image<int> im = output.realize(1024, 1024);
    auto func = [](int x, int y) { return 3*x + 2*y; };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int multi_folds_wrapper_test() {
    Func source("source"), img_in_g_in_g, img_in_g, img_in_g_in_g_in_h, img_in_g_in_g_in_h_in_h, g("g"), h("h");
    Var x("x"), y("y");

    source(x, y) = 2*x + 3*y;
    ImageParam img(Int(32), 2, "img");
    Image<int> buf = source.realize(1024, 1024);
    img.set(buf);

    Func img_f = img;
    img_f.compute_root();

    g(x, y) = img(y, x);

    Var xi("xi"), yi("yi");
    g.compute_root().tile(x, y, xi, yi, 8, 8);

    img_in_g = img.in(g).compute_root().tile(_0, _1, xi, yi, 8, 8).vectorize(xi).unroll(yi);
    img_in_g_in_g = img_in_g.in(g).compute_root().tile(_0, _1, xi, yi, 8, 8).unroll(xi).unroll(yi);

    h(x, y) = img_in_g_in_g(y, x);
    img_in_g_in_g_in_h = img_in_g_in_g.in(h).compute_at(h, x).vectorize(_0).unroll(_1);
    img_in_g_in_g_in_h_in_h = img_in_g_in_g_in_h.in(h).compute_at(h, x).unroll(_0).unroll(_1);
    h.compute_root().tile(x, y, xi, yi, 8, 8);

    map<string, Function> funcs = {
        {img_f.name(), img_f.function()},
        {g.name(), g.function()},
        {h.name(), h.function()},
        {img_in_g_in_g.name(), img_in_g_in_g.function()},
        {img_in_g.name(), img_in_g.function()},
        {img_in_g_in_g_in_h.name(), img_in_g_in_g_in_h.function()},
        {img_in_g_in_g_in_h_in_h.name(), img_in_g_in_g_in_h_in_h.function()},
    };

    {
        // Check the call graphs.
        Module m = g.compile_to_module({g.infer_arguments()});
        CheckCalls c(funcs);
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {g.name(), {img_in_g_in_g.name()}},
            {img_in_g_in_g.name(), {img_in_g.name()}},
            {img_in_g.name(), {img_f.name()}},
            {img_f.name(), {img.name()}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }

        Image<int> im = g.realize(1024, 1024);
        auto func = [](int x, int y) { return 3*x + 2*y; };
        if (check_image(im, func)) {
            return -1;
        }
    }

    {
        // Check the call graphs.
        Module m = h.compile_to_module({h.infer_arguments()});
        CheckCalls c(funcs);
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {h.name(), {img_in_g_in_g_in_h_in_h.name()}},
            {img_in_g_in_g_in_h_in_h.name(), {img_in_g_in_g_in_h.name()}},
            {img_in_g_in_g_in_h.name(), {img_in_g_in_g.name()}},
            {img_in_g_in_g.name(), {img_in_g.name()}},
            {img_in_g.name(), {img_f.name()}},
            {img_f.name(), {img.name()}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }

        Image<int> im = h.realize(1024, 1024);
        auto func = [](int x, int y) { return 3*x + 2*y; };
        if (check_image(im, func)) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    printf("Running calling wrap no op test\n");
    if (calling_wrap_no_op_test() != 0) {
        return -1;
    }

    printf("Running func wrap test\n");
    if (func_wrap_test() != 0) {
        return -1;
    }

    printf("Running multiple funcs sharing wrapper test\n");
    if (multiple_funcs_sharing_wrapper_test() != 0) {
        return -1;
    }

    printf("Running global wrap test\n");
    if (global_wrap_test() != 0) {
        return -1;
    }

    printf("Running update is defined after wrap test\n");
    if (update_defined_after_wrap_test() != 0) {
        return -1;
    }

    printf("Running rdom wrapper test\n");
    if (rdom_wrapper_test() != 0) {
        return -1;
    }

    printf("Running global + custom wrapper test\n");
    if (global_and_custom_wrap_test() != 0) {
        return -1;
    }

    printf("Running wrapper depend on mutated func test\n");
    if (wrapper_depend_on_mutated_func_test() != 0) {
        return -1;
    }

    printf("Running wrapper on wrapper test\n");
    if (wrapper_on_wrapper_test() != 0) {
        return -1;
    }

    printf("Running wrapper on rdom predicate test\n");
    if (wrapper_on_rdom_predicate_test() != 0) {
        return -1;
    }

    printf("Running two fold wrapper test\n");
    if (two_fold_wrapper_test() != 0) {
        return -1;
    }

    printf("Running multi folds wrapper test\n");
    if (multi_folds_wrapper_test() != 0) {
        return -1;
    }

    printf("Success!\n");
    return 0;
}
