/*++
Copyright (c) 2015 Microsoft Corporation

Module Name:

    qsat.cpp

Abstract:

    Quantifier Satisfiability Solver.

Author:

    Nikolaj Bjorner (nbjorner) 2015-5-28

Revision History:

Notes:


--*/

#include "smt_kernel.h"
#include "qe_mbp.h"
#include "smt_params.h"
#include "ast_util.h"
#include "quant_hoist.h"
#include "ast_pp.h" 
#include "model_v2_pp.h"
#include "qsat.h"
#include "expr_abstract.h"
#include "qe.h"


namespace qe {

    pred_abs::pred_abs(ast_manager& m):
        m(m),
        m_asms(m),
        m_trail(m),
        m_fmc(alloc(filter_model_converter, m))
    {
    }

    filter_model_converter* pred_abs::fmc() { 
        return m_fmc.get(); 
    }

    void pred_abs::reset() {
        m_trail.reset();
        dec_keys<expr>(m_pred2lit);
        dec_keys<app>(m_lit2pred);
        dec_keys<app>(m_asm2pred);
        dec_keys<expr>(m_pred2asm);
        m_lit2pred.reset();
        m_pred2lit.reset();
        m_asm2pred.reset();
        m_pred2asm.reset();
        m_elevel.reset();
        m_asms.reset();
        m_asms_lim.reset();
        m_preds.reset();
    }

    max_level pred_abs::compute_level(app* e) {
        unsigned sz0 = todo.size();
        todo.push_back(e);        
        while (sz0 != todo.size()) {
            app* a = to_app(todo.back());
            if (m_elevel.contains(a)) {
                todo.pop_back();
                continue;
            }
            max_level lvl, lvl0;
            bool has_new = false;
            if (m_flevel.find(a->get_decl(), lvl)) {
                lvl0.merge(lvl);
            }
            for (unsigned i = 0; i < a->get_num_args(); ++i) {
                app* arg = to_app(a->get_arg(i));
                if (m_elevel.find(arg, lvl)) {
                    lvl0.merge(lvl);
                }
                else {
                    todo.push_back(arg);
                    has_new = true;
                }
            }
            if (!has_new) {
                m_elevel.insert(a, lvl0);
                todo.pop_back();
            }
        }
        return m_elevel.find(e);
    }
    
    void pred_abs::add_pred(app* p, app* lit) {
        m.inc_ref(p);
        m_pred2lit.insert(p, lit);
        add_lit(p, lit);
    }

    void pred_abs::add_lit(app* p, app* lit) {
        if (!m_lit2pred.contains(lit)) {
            m.inc_ref(lit);
            m_lit2pred.insert(lit, p);        
        }
    }

    void pred_abs::add_asm(app* p, expr* assum) {
        SASSERT(!m_asm2pred.contains(assum));
        m.inc_ref(p);
        m.inc_ref(assum);
        m_asm2pred.insert(assum, p);
        m_pred2asm.insert(p, assum);
    }
    
    void pred_abs::push() {
        m_asms_lim.push_back(m_asms.size());
    }

    void pred_abs::pop(unsigned num_scopes) {
        unsigned l = m_asms_lim.size() - num_scopes;
        m_asms.resize(m_asms_lim[l]);
        m_asms_lim.shrink(l);            
    }
        
    void pred_abs::insert(app* a, max_level const& lvl) {
        unsigned l = lvl.max();
        if (l == UINT_MAX) l = 0;
        while (m_preds.size() <= l) {
            m_preds.push_back(app_ref_vector(m));
        }
        m_preds[l].push_back(a);            
    }

    bool pred_abs::is_predicate(app* a, unsigned l) {
        max_level lvl1;
        return m_flevel.find(a->get_decl(), lvl1) && lvl1.max() < l;
    }

    void pred_abs::get_assumptions(model* mdl, expr_ref_vector& asms) {
        unsigned level = m_asms_lim.size();
        if (level > m_preds.size()) {
            level = m_preds.size();
        }
        if (level == 0) {
            return;
        }
        if (!mdl) {
            asms.append(m_asms);
            return;
        }
        expr_ref val(m);
        for (unsigned j = 0; j < m_preds[level - 1].size(); ++j) {
            app* p = m_preds[level - 1][j].get();
            TRACE("qe", tout << "process level: " << level - 1 << ": " << mk_pp(p, m) << "\n";);
            
            VERIFY(mdl->eval(p, val));
            
            if (m.is_false(val)) {
                m_asms.push_back(m.mk_not(p));
            }
            else {
                SASSERT(m.is_true(val));
                m_asms.push_back(p);
            }
        }
        asms.append(m_asms);
        
        for (unsigned i = level + 1; i < m_preds.size(); i += 2) {
            for (unsigned j = 0; j < m_preds[i].size(); ++j) {
                app* p = m_preds[i][j].get();
                max_level lvl = m_elevel.find(p);
                bool use = 
                    (lvl.m_fa == i && (lvl.m_ex == UINT_MAX || lvl.m_ex < level)) ||
                    (lvl.m_ex == i && (lvl.m_fa == UINT_MAX || lvl.m_fa < level));
                if (use) {
                    VERIFY(mdl->eval(p, val));
                    if (m.is_false(val)) {
                        asms.push_back(m.mk_not(p));
                    }
                    else {
                        asms.push_back(p);
                    }
                }
            }
        }
        TRACE("qe", tout << "level: " << level << "\n";
              model_v2_pp(tout, *mdl);
              display(tout, asms););
    }
    
    void pred_abs::set_expr_level(app* v, max_level const& lvl) {
        m_elevel.insert(v, lvl);
    }

    void pred_abs::set_decl_level(func_decl* f, max_level const& lvl) {
        m_flevel.insert(f, lvl);
    }

    void pred_abs::abstract_atoms(expr* fml, expr_ref_vector& defs) {
        max_level level;
        abstract_atoms(fml, level, defs);
    }
    /** 
        \brief create propositional abstraction of formula by replacing atomic sub-formulas by fresh 
        propositional variables, and adding definitions for each propositional formula on the side.
        Assumption is that the formula is quantifier-free.
    */
    void pred_abs::abstract_atoms(expr* fml, max_level& level, expr_ref_vector& defs) {
        expr_mark mark;
        ptr_vector<expr> args;
        app_ref r(m), eq(m);
        app* p;
        unsigned sz0 = todo.size();
        todo.push_back(fml);
        m_trail.push_back(fml);
        max_level l;
        while (sz0 != todo.size()) {
            app* a = to_app(todo.back());
            todo.pop_back();
            if (mark.is_marked(a)) {
                continue;
            }
            
            mark.mark(a);
            if (m_lit2pred.find(a, p)) {
                TRACE("qe", tout << mk_pp(a, m) << " " << mk_pp(p, m) << "\n";);
                level.merge(m_elevel.find(p));
                continue;
            }
            
            if (is_uninterp_const(a) && m.is_bool(a)) {
                l = m_elevel.find(a);
                level.merge(l);                
                if (!m_pred2lit.contains(a)) {
                    add_pred(a, a);
                    insert(a, l);
                }
                continue;
            }
            
            unsigned sz = a->get_num_args();
            for (unsigned i = 0; i < sz; ++i) {
                expr* f = a->get_arg(i);
                if (!mark.is_marked(f)) {
                    todo.push_back(f);
                }
            } 
            
            bool is_boolop = 
                (a->get_family_id() == m.get_basic_family_id()) &&
                (!m.is_eq(a)       || m.is_bool(a->get_arg(0))) && 
                (!m.is_distinct(a) || m.is_bool(a->get_arg(0)));
            
            if (!is_boolop && m.is_bool(a)) {
                TRACE("qe", tout << mk_pp(a, m) << "\n";);
                r = fresh_bool("p");
                max_level l = compute_level(a);
                add_pred(r, a);
                m_elevel.insert(r, l);
                eq = m.mk_eq(r, a);
                defs.push_back(eq);
                if (!is_predicate(a, l.max())) {
                    insert(r, l);
                }
                level.merge(l);
            }
        }
    }

    app_ref pred_abs::fresh_bool(char const* name) {
        app_ref r(m.mk_fresh_const(name, m.mk_bool_sort()), m);
        m_fmc->insert(r->get_decl());
        return r;
    }


    // optional pass to replace atoms by predicates 
    // so that SMT core works on propositional
    // abstraction only.
    expr_ref pred_abs::mk_abstract(expr* fml) {
        expr_ref_vector trail(m), args(m);
        obj_map<expr, expr*> cache;
        app* b;
        expr_ref r(m);
        unsigned sz0 = todo.size();
        todo.push_back(fml);
        while (sz0 != todo.size()) {
            app* a = to_app(todo.back());
            if (cache.contains(a)) {
                todo.pop_back();
                continue;
            }
            if (m_lit2pred.find(a, b)) {
                cache.insert(a, b);
                todo.pop_back();
                continue;
            }
            unsigned sz = a->get_num_args();
            bool diff = false;
            args.reset();
            for (unsigned i = 0; i < sz; ++i) {
                expr* f = a->get_arg(i), *f1;
                if (cache.find(f, f1)) {
                    args.push_back(f1);
                    diff |= f != f1;
                }
                else {
                    todo.push_back(f);
                }
            } 
            if (sz == args.size()) {
                if (diff) {
                    r = m.mk_app(a->get_decl(), sz, args.c_ptr());
                    trail.push_back(r);
                }
                else {
                    r = a;
                }
                cache.insert(a, r);
                todo.pop_back();
            }
        }
        return expr_ref(cache.find(fml), m);
    }

    expr_ref pred_abs::mk_assumption_literal(expr* a, model* mdl, max_level const& lvl, expr_ref_vector& defs) {
        expr_ref A(m);
        A = pred2asm(a);
        a = A;
        app_ref p(m);
        expr_ref q(m), fml(m);
        app *b;
        expr *c, *d;
        max_level lvl2;
        TRACE("qe", tout << mk_pp(a, m) << " " << lvl << "\n";);
        if (m_asm2pred.find(a, b)) {
            q = b;
        }
        else if (m.is_not(a, c) && m_asm2pred.find(c, b)) {
            q = m.mk_not(b);
        }
        else if (m_pred2asm.find(a, d)) {
            q = a;
        }
        else if (m.is_not(a, c) && m_pred2asm.find(c, d)) {
            q = a;
        }
        else {
            p = fresh_bool("def");
            if (m.is_not(a, a)) {
                if (mdl) 
                    mdl->register_decl(p->get_decl(), m.mk_false());
                q = m.mk_not(p);
            }
            else {
                if (mdl)
                    mdl->register_decl(p->get_decl(), m.mk_true());
                q = p;
            }
            m_elevel.insert(p, lvl);
            insert(p, lvl);
            fml = a;
            abstract_atoms(fml, lvl2, defs);
            fml = mk_abstract(fml);
            defs.push_back(m.mk_eq(p, fml));
            add_asm(p, a);
            TRACE("qe", tout << mk_pp(a, m) << " |-> " << p << "\n";);
        }
        return q;
    }

    void pred_abs::mk_concrete(expr_ref_vector& fmls, obj_map<expr,expr*> const& map) {
        obj_map<expr,expr*> cache;
        expr_ref_vector trail(m);
        expr* p;
        app_ref r(m);
        ptr_vector<expr> args;
        unsigned sz0 = todo.size();
        todo.append(fmls.size(), (expr*const*)fmls.c_ptr());
        while (sz0 != todo.size()) {
            app* a = to_app(todo.back());
            if (cache.contains(a)) {
                todo.pop_back();
                continue;
            }
            if (map.find(a, p)) {
                cache.insert(a, p);
                todo.pop_back();
                continue;
            }
            unsigned sz = a->get_num_args();
            args.reset();
            bool diff = false;
            for (unsigned i = 0; i < sz; ++i) {
                expr* f = a->get_arg(i), *f1;
                if (cache.find(f, f1)) {
                    args.push_back(f1);
                    diff |= f != f1;
                }
                else {
                    todo.push_back(f);
                }
            } 
            if (args.size() == sz) {
                if (diff) {
                    r = m.mk_app(a->get_decl(), sz, args.c_ptr());
                }
                else {
                    r = to_app(a);
                }
                cache.insert(a, r);
                trail.push_back(r);
                todo.pop_back();
            }
        }
        for (unsigned i = 0; i < fmls.size(); ++i) {
            fmls[i] = to_app(cache.find(fmls[i].get()));
        }        
    }
    
    void pred_abs::pred2lit(expr_ref_vector& fmls) {
        mk_concrete(fmls, m_pred2lit);
    }

    expr_ref pred_abs::pred2asm(expr* fml) {
        expr_ref_vector fmls(m);
        fmls.push_back(fml);
        mk_concrete(fmls, m_pred2asm);
        return mk_and(fmls);
    }

    void pred_abs::collect_statistics(statistics& st) const {
        st.update("qsat num predicates", m_pred2lit.size());
    }
        
    void pred_abs::display(std::ostream& out) const {
        out << "pred2lit:\n";
        obj_map<expr, expr*>::iterator it = m_pred2lit.begin(), end = m_pred2lit.end();
        for (; it != end; ++it) {
            out << mk_pp(it->m_key, m) << " |-> " << mk_pp(it->m_value, m) << "\n";
        }
        for (unsigned i = 0; i < m_preds.size(); ++i) {
            out << "level " << i << "\n";
            for (unsigned j = 0; j < m_preds[i].size(); ++j) {
                app* p = m_preds[i][j];
                expr* e;
                if (m_pred2lit.find(p, e)) {
                    out << mk_pp(p, m) << " := " << mk_pp(e, m) << "\n";
                }
                else {
                    out << mk_pp(p, m) << "\n";
                }
            }
        }            
    }        
    
    void pred_abs::display(std::ostream& out, expr_ref_vector const& asms) const {
        max_level lvl;       
        for (unsigned i = 0; i < asms.size(); ++i) {
            expr* e = asms[i];
            bool is_not = m.is_not(asms[i], e);
            out << mk_pp(asms[i], m);
            if (m_elevel.find(e, lvl)) {
                lvl.display(out << " - ");
            }
            if (m_pred2lit.find(e, e)) {
                out << " : " << (is_not?"!":"") << mk_pp(e, m);
            }
            out << "\n";
        }
    }

    void pred_abs::get_free_vars(expr* fml, app_ref_vector& vars) {
        ast_fast_mark1 mark;
        unsigned sz0 = todo.size();
        todo.push_back(fml);
        while (sz0 != todo.size()) {
            expr* e = todo.back();
            todo.pop_back();
            if (mark.is_marked(e) || is_var(e)) {
                continue;
            }
            mark.mark(e);
            if (is_quantifier(e)) {
                todo.push_back(to_quantifier(e)->get_expr());
                continue;
            }
            SASSERT(is_app(e));
            app* a = to_app(e);
            if (is_uninterp_const(a)) { // TBD generalize for uninterpreted functions.
                vars.push_back(a);
            }
            for (unsigned i = 0; i < a->get_num_args(); ++i) {
                todo.push_back(a->get_arg(i));
            }
        }
    }
    
    class qsat : public tactic {
        
        struct stats {
            unsigned m_num_rounds;        
            stats() { reset(); }
            void reset() { memset(this, 0, sizeof(*this)); }
        };
        
        class kernel {
            ast_manager& m;
            smt_params   m_smtp;
            smt::kernel  m_kernel;
            
        public:
            kernel(ast_manager& m):
                m(m),
                m_kernel(m, m_smtp)
            {
                m_smtp.m_model = true;
                m_smtp.m_relevancy_lvl = 0;
            }
            
            smt::kernel& k() { return m_kernel; }
            smt::kernel const& k() const { return m_kernel; }
            
            void assert_expr(expr* e) {
                m_kernel.assert_expr(e);
            }
            
            void get_core(expr_ref_vector& core) {
                unsigned sz = m_kernel.get_unsat_core_size();
                core.reset();
                for (unsigned i = 0; i < sz; ++i) {
                    core.push_back(m_kernel.get_unsat_core_expr(i));
                }
                TRACE("qe", tout << "core: " << core << "\n";
                      m_kernel.display(tout);
                      tout << "\n";
                      );
            }
        };
        
        ast_manager&               m;
        params_ref                 m_params;
        stats                      m_stats;
        statistics                 m_st;
        qe::mbp                    m_mbp;
        kernel                     m_fa;
        kernel                     m_ex;
        pred_abs                   m_pred_abs;
        expr_ref_vector            m_answer;
        expr_ref_vector            m_asms;
        vector<app_ref_vector>     m_vars;       // variables from alternating prefixes.
        unsigned                   m_level;
        model_ref                  m_model;
        volatile bool              m_cancel;
        bool                       m_qelim;       // perform quantifier elimination
        bool                       m_force_elim;  // force elimination of variables during projection.
        app_ref_vector             m_avars;       // variables to project
        app_ref_vector             m_free_vars;

        
        /**
           \brief check alternating satisfiability.
           Even levels are existential, odd levels are universal.
        */
        lbool check_sat() {        
            while (true) {
                ++m_stats.m_num_rounds;
                check_cancel();
                expr_ref_vector asms(m_asms);
                m_pred_abs.get_assumptions(m_model.get(), asms);
                smt::kernel& k = get_kernel(m_level).k();
                lbool res = k.check(asms);
                switch (res) {
                case l_true:
                    k.get_model(m_model);
                    TRACE("qe", k.display(tout); display(tout << "\n", *m_model.get()); display(tout, asms); );
                    push();
                break;
                case l_false:
                    switch (m_level) {
                    case 0: return l_false;
                    case 1: 
                        if (!m_qelim) return l_true; 
                        if (m_model.get()) {
                            project_qe(asms);
                        }
                        else {
                            pop(1);
                        }
                        break;
                    default: 
                        if (m_model.get()) {
                            project(asms); 
                        }
                        else {
                            pop(1);
                        }
                        break;
                    }
                    break;
                case l_undef:
                    return res;
                }
            }
            return l_undef;
        }

        kernel& get_kernel(unsigned j) {        
            if (is_exists(j)) {
                return m_ex; 
            }
            else {
                return m_fa;
            }
        }
        
        bool is_exists(unsigned level) const {
            return (level % 2) == 0;
        }
        
        bool is_forall(unsigned level) const {
            return is_exists(level+1);
        }
        
        void push() {
            m_level++;
            m_pred_abs.push();
        }
        
        void pop(unsigned num_scopes) {
            m_model.reset();
            SASSERT(num_scopes <= m_level);
            m_pred_abs.pop(num_scopes);
            m_level -= num_scopes;
        }
        
        void reset() {
            m_st.reset();        
            m_fa.k().collect_statistics(m_st);
            m_ex.k().collect_statistics(m_st);        
            m_pred_abs.collect_statistics(m_st);
            m_level = 0;
            m_answer.reset();
            m_asms.reset();
            m_pred_abs.reset();
            m_vars.reset();
            m_model = 0;
            m_fa.k().reset();
            m_ex.k().reset();        
            m_cancel = false;
            m_free_vars.reset();
        }    
        
        /**
           \brief create a quantifier prefix formula.
        */
        void hoist(expr_ref& fml) {
            quantifier_hoister hoist(m);
            app_ref_vector vars(m);
            bool is_forall = false;        
            m_pred_abs.get_free_vars(fml, vars);
            m_vars.push_back(vars);
            vars.reset();
            if (m_qelim) {
                is_forall = true;
                hoist.pull_quantifier(is_forall, fml, vars);
                m_vars.push_back(vars);
            }
            else {
                hoist.pull_quantifier(is_forall, fml, vars);
                m_vars.back().append(vars);
            }
            do {
                is_forall = !is_forall;
                vars.reset();
                hoist.pull_quantifier(is_forall, fml, vars);
                m_vars.push_back(vars);
            }
            while (!vars.empty());
            SASSERT(m_vars.back().empty()); 
            initialize_levels();
            TRACE("qe", tout << fml << "\n";);
        }

        void initialize_levels() {
            // initialize levels.
            for (unsigned i = 0; i < m_vars.size(); ++i) {
                max_level lvl;
                if (is_exists(i)) {
                    lvl.m_ex = i;
                }
                else {
                    lvl.m_fa = i;
                }
                for (unsigned j = 0; j < m_vars[i].size(); ++j) {
                    m_pred_abs.set_expr_level(m_vars[i][j].get(), lvl);
                }
            }
        }
        
        void get_core(expr_ref_vector& core, unsigned level) {
            get_kernel(level).get_core(core);
            m_pred_abs.pred2lit(core);
        }
        
        void check_cancel() {
            if (m_cancel) {
                throw tactic_exception(TACTIC_CANCELED_MSG);
            }
        }
        
        void display(std::ostream& out) const {
            out << "level: " << m_level << "\n";
            for (unsigned i = 0; i < m_vars.size(); ++i) {
                for (unsigned j = 0; j < m_vars[i].size(); ++j) {
                    expr* v = m_vars[i][j];
                    out << mk_pp(v, m) << " ";
                }
                out << "\n";
            }
            m_pred_abs.display(out);
        }
        
        void display(std::ostream& out, model& model) const {
            display(out);
            model_v2_pp(out, model);
        }
        
        void display(std::ostream& out, expr_ref_vector const& asms) const {
            m_pred_abs.display(out, asms);
        }
        
        void add_assumption(expr* fml) {
            app_ref b = m_pred_abs.fresh_bool("b");        
            m_asms.push_back(b);
            m_ex.assert_expr(m.mk_eq(b, fml));
            m_pred_abs.add_pred(b, to_app(fml));
            max_level lvl;
            m_pred_abs.set_expr_level(b, lvl);
        }
        
        void project_qe(expr_ref_vector& core) {
            SASSERT(m_level == 1);
            expr_ref fml(m);
            model& mdl = *m_model.get();
            get_core(core, m_level);
            get_vars(m_level);
            m_mbp(m_force_elim, m_avars, mdl, core);
            fml = negate_core(core);
            add_assumption(fml);
            m_answer.push_back(fml);
            m_free_vars.append(m_avars);
            pop(1);
        }
                
        void project(expr_ref_vector& core) {
            get_core(core, m_level);
            TRACE("qe", display(tout); display(tout << "core\n", core););
            SASSERT(m_level >= 2);
            expr_ref fml(m); 
            expr_ref_vector defs(m);
            max_level level;
            model& mdl = *m_model.get();
            
            get_vars(m_level-1);
            m_mbp(m_force_elim, m_avars, mdl, core);
            m_free_vars.append(m_avars);
            fml = negate_core(core);
            unsigned num_scopes = 0;
            
            m_pred_abs.abstract_atoms(fml, level, defs);
            m_ex.assert_expr(mk_and(defs));
            m_fa.assert_expr(mk_and(defs));
            if (level.max() == UINT_MAX) {
                num_scopes = 2*(m_level/2);
            }
            else if (m_qelim && !m_force_elim) {
                num_scopes = 2;
            }
            else {
                SASSERT(level.max() + 2 <= m_level);
                num_scopes = m_level - level.max();
                SASSERT(num_scopes >= 2);
            }
            
            TRACE("qe", tout << "backtrack: " << num_scopes << "\nproject:\n" << core << "\n|->\n" << fml << "\n";);
            pop(num_scopes); 
            if (m_level == 0 && m_qelim) {
                add_assumption(fml);
            }
            else {
                fml = m_pred_abs.mk_abstract(fml);
                get_kernel(m_level).assert_expr(fml);
            }
        }
        
        void get_vars(unsigned level) {
            m_avars.reset();
            for (unsigned i = level; i < m_vars.size(); ++i) {
                m_avars.append(m_vars[i]);
            }
        } 
        
        expr_ref negate_core(expr_ref_vector& core) {
            return ::push_not(::mk_and(core));
        }
        
        expr_ref elim_rec(expr* fml) {
            expr_ref tmp(m);
            expr_ref_vector     trail(m);
            obj_map<expr,expr*> visited;
            ptr_vector<expr>    todo;
            trail.push_back(fml);
            todo.push_back(fml);
            expr* e = 0, *r = 0;
            
            while (!todo.empty()) {
                check_cancel();

                e = todo.back();
                if (visited.contains(e)) {
                    todo.pop_back();
                    continue;            
                }
                
                switch(e->get_kind()) {
                case AST_APP: {
                    app* a = to_app(e);
                    expr_ref_vector args(m);
                    unsigned num_args = a->get_num_args();
                    bool all_visited = true;
                    for (unsigned i = 0; i < num_args; ++i) {
                        if (visited.find(a->get_arg(i), r)) {
                            args.push_back(r);
                        }
                        else {
                            todo.push_back(a->get_arg(i));
                            all_visited = false;
                        }
                    }
                    if (all_visited) {
                        r = m.mk_app(a->get_decl(), args.size(), args.c_ptr());
                        todo.pop_back();
                        trail.push_back(r);
                        visited.insert(e, r);
                    }
                    break;
                }
                case AST_QUANTIFIER: {
                    app_ref_vector vars(m);
                    quantifier* q = to_quantifier(e);
                    bool is_fa = q->is_forall();
                    tmp = q->get_expr();
                    extract_vars(q, tmp, vars);
                    TRACE("qe", tout << vars << " " << mk_pp(q, m) << " " << tmp << "\n";);
                    tmp = elim_rec(tmp);
                    if (is_fa) {
                        tmp = ::push_not(tmp);
                    }
                    tmp = elim(vars, tmp);
                    if (is_fa) {
                        tmp = ::push_not(tmp);
                    }
                    trail.push_back(tmp);
                    visited.insert(e, tmp);
                    todo.pop_back();
                    break;
                }
                default:
                    UNREACHABLE();
                    break;
                }        
            }    
            VERIFY (visited.find(fml, e));
            return expr_ref(e, m);
        }
        
        expr_ref elim(app_ref_vector const& vars, expr* _fml) {
            expr_ref fml(_fml, m);
            reset();
            m_vars.push_back(app_ref_vector(m));
            m_vars.push_back(vars);
            initialize_levels();
            fml = push_not(fml);            

            TRACE("qe", tout << vars << " " << fml << "\n";);
            expr_ref_vector defs(m);
            m_pred_abs.abstract_atoms(fml, defs);
            fml = m_pred_abs.mk_abstract(fml);
            m_ex.assert_expr(mk_and(defs));
            m_fa.assert_expr(mk_and(defs));
            m_ex.assert_expr(fml);
            m_fa.assert_expr(m.mk_not(fml));
            TRACE("qe", tout << "ex: " << fml << "\n";);
            lbool is_sat = check_sat();
            fml = ::mk_and(m_answer);
            TRACE("qe", tout << "ans: " << fml << "\n";
                  tout << "Free vars: " << m_free_vars << "\n";);            
            if (is_sat == l_false) {
                obj_hashtable<app> vars;
                for (unsigned i = 0; i < m_free_vars.size(); ++i) {
                    app* v = m_free_vars[i].get();
                    if (vars.contains(v)) {
                        m_free_vars[i] = m_free_vars.back();
                        m_free_vars.pop_back();
                        --i;
                    }
                    else {
                        vars.insert(v);
                    }
                }
                fml = mk_exists(m, m_free_vars.size(), m_free_vars.c_ptr(), fml);
                return fml;
            }
            else {
                return expr_ref(_fml, m);
            }
        }

    public:
        
        qsat(ast_manager& m, params_ref const& p, bool qelim, bool force_elim):
            m(m),
            m_mbp(m),
            m_fa(m),
            m_ex(m),
            m_pred_abs(m),
            m_answer(m),
            m_asms(m),
            m_level(0),
            m_cancel(false),
            m_qelim(qelim),
            m_force_elim(force_elim),
            m_avars(m),
            m_free_vars(m)
        {
            reset();
        }
        
        virtual ~qsat() {
            reset();
        }
        
        void updt_params(params_ref const & p) {
        }
        
        void collect_param_descrs(param_descrs & r) {
        }

        
        void operator()(/* in */  goal_ref const & in, 
                        /* out */ goal_ref_buffer & result, 
                        /* out */ model_converter_ref & mc, 
                        /* out */ proof_converter_ref & pc,
                        /* out */ expr_dependency_ref & core) {
            tactic_report report("qsat-tactic", *in);
            ptr_vector<expr> fmls;
            expr_ref_vector defs(m);
            expr_ref fml(m);
            mc = 0; pc = 0; core = 0;
            in->get_formulas(fmls);
            fml = mk_and(m, fmls.size(), fmls.c_ptr());
            
            // for now:
            // fail if cores.  (TBD)
            // fail if proofs. (TBD)
            
            if (!m_force_elim) {
                fml = elim_rec(fml);
                in->reset();
                in->inc_depth();
                in->assert_expr(fml);                
                result.push_back(in.get());
                return;
            }
                
            reset();
            TRACE("qe", tout << fml << "\n";);
            if (m_qelim) {
                fml = push_not(fml);
            }
            hoist(fml);
            m_pred_abs.abstract_atoms(fml, defs);
            fml = m_pred_abs.mk_abstract(fml);
            m_ex.assert_expr(mk_and(defs));
            m_fa.assert_expr(mk_and(defs));
            m_ex.assert_expr(fml);
            m_fa.assert_expr(m.mk_not(fml));
            TRACE("qe", tout << "ex: " << fml << "\n";);
            lbool is_sat = check_sat();
            
            switch (is_sat) {
            case l_false:
                in->reset();
                in->inc_depth();
                if (m_qelim) {
                    fml = ::mk_and(m_answer);
                    in->assert_expr(fml);
                }
                else {
                    in->assert_expr(m.mk_false());
                }
                result.push_back(in.get());
                break;
            case l_true:
                in->reset();
                in->inc_depth();
                result.push_back(in.get());
                if (in->models_enabled()) {
                    mc = model2model_converter(m_model.get());
                    mc = concat(m_pred_abs.fmc(), mc.get());
                }
                break;
            case l_undef:
                result.push_back(in.get());
                std::string s = m_ex.k().last_failure_as_string();
                if (s == "ok") {
                    s = m_fa.k().last_failure_as_string();
                }
                throw tactic_exception(s.c_str()); 
            }        
        }
        
        void collect_statistics(statistics & st) const {
            st.copy(m_st);
            st.update("qsat num rounds", m_stats.m_num_rounds); 
            m_pred_abs.collect_statistics(st);
        }
        
        void reset_statistics() {
            m_stats.reset();
            m_fa.k().reset_statistics();
            m_ex.k().reset_statistics();        
        }
        
        void cleanup() {
            reset();
            set_cancel(false);
        }
        
        void set_logic(symbol const & l) {
        }
        
        void set_progress_callback(progress_callback * callback) {
        }
        
        tactic * translate(ast_manager & m) {
            return alloc(qsat, m, m_params, m_qelim, m_force_elim);
        }
        
        virtual void set_cancel(bool f) {
            m_fa.k().set_cancel(f);        
            m_ex.k().set_cancel(f);        
            m_cancel = f;
        }
        
    };
    
};

tactic * mk_qsat_tactic(ast_manager& m, params_ref const& p) {
    return alloc(qe::qsat, m, p, false, true);
}

tactic * mk_qe2_tactic(ast_manager& m, params_ref const& p) {   
    return alloc(qe::qsat, m, p, true, true);
}

tactic * mk_qe_rec_tactic(ast_manager& m, params_ref const& p) {   
    return alloc(qe::qsat, m, p, true, false);
}

