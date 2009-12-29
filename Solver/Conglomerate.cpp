/***********************************************************************************
CryptoMiniSat -- Copyright (c) 2009 Mate Soos

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************************************/

#include "Conglomerate.h"
#include "Solver.h"
#include "VarReplacer.h"
#include "ClauseCleaner.h"

#include <utility>
#include <algorithm>
using std::make_pair;

//#define VERBOSE_DEBUG

#ifdef VERBOSE_DEBUG
#include <iostream>
using std::cout;
using std::endl;
#endif

Conglomerate::Conglomerate(Solver *_s) :
    S(_s)
{}

Conglomerate::~Conglomerate()
{
    for(uint i = 0; i < calcAtFinish.size(); i++)
        free(calcAtFinish[i]);
}

const vector<bool>& Conglomerate::getRemovedVars() const
{
    return removedVars;
}

const vec<XorClause*>& Conglomerate::getCalcAtFinish() const
{
    return calcAtFinish;
}

vec<XorClause*>& Conglomerate::getCalcAtFinish()
{
    return calcAtFinish;
}

void Conglomerate::fillVarToXor()
{
    blocked.clear();
    varToXor.clear();
    
    blocked.resize(S->nVars(), false);
    for (Clause *const*it = S->clauses.getData(), *const*end = it + S->clauses.size(); it != end; it++) {
        const Clause& c = **it;
        for (const Lit* a = &c[0], *end = a + c.size(); a != end; a++) {
            blocked[a->var()] = true;
        }
    }
    
    for (Lit* it = &(S->trail[0]), *end = it + S->trail.size(); it != end; it++)
        blocked[it->var()] = true;
    
    const vec<Clause*>& clauses = S->varReplacer->getClauses();
    for (Clause *const*it = clauses.getData(), *const*end = it + clauses.size(); it != end; it++) {
        const Clause& c = **it;
        for (const Lit* a = &c[0], *end = a + c.size(); a != end; a++) {
            blocked[a->var()] = true;
        }
    }
    
    uint i = 0;
    for (XorClause* const* it = S->xorclauses.getData(), *const*end = it + S->xorclauses.size(); it != end; it++, i++) {
        const XorClause& c = **it;
        for (const Lit * a = &c[0], *end = a + c.size(); a != end; a++) {
            if (!blocked[a->var()])
                varToXor[a->var()].push_back(make_pair(*it, i));
        }
    }
}

void Conglomerate::process_clause(XorClause& x, const uint32_t num, Var remove_var, vec<Lit>& vars) {
    for (const Lit* a = &x[0], *end = a + x.size(); a != end; a++) {
        Var var = a->var();
        if (var != remove_var) {
            vars.push(Lit(var, false));
            varToXorMap::iterator finder = varToXor.find(var);
            if (finder != varToXor.end()) {
                vector<pair<XorClause*, uint32_t> >::iterator it =
                    std::find(finder->second.begin(), finder->second.end(), make_pair(&x, num));
                finder->second.erase(it);
            }
        }
    }
}

uint Conglomerate::conglomerateXors()
{
    if (S->xorclauses.size() == 0)
        return 0;
    
    #ifdef VERBOSE_DEBUG
    cout << "Finding conglomerate xors started" << endl;
    #endif
    
    S->clauseCleaner->cleanClauses(S->xorclauses, ClauseCleaner::xorclauses);
    
    toRemove.clear();
    toRemove.resize(S->xorclauses.size(), false);
    
    fillVarToXor();
    
    uint found = 0;
    while(varToXor.begin() != varToXor.end()) {
        varToXorMap::iterator it = varToXor.begin();
        const vector<pair<XorClause*, uint32_t> >& c = it->second;
        const uint& var = it->first;
        
        //We blocked the var during dealWithNewClause (it was in a 2-long xor-clause)
        if (blocked[var]) {
            varToXor.erase(it);
            continue;
        }
        
        S->setDecisionVar(var, false);
        removedVars[var] = true;
        
        if (c.size() == 0) {
            varToXor.erase(it);
            continue;
        }
        
        #ifdef VERBOSE_DEBUG
        cout << "--- New conglomerate set ---" << endl;
        #endif
        
        XorClause& x = *(c[0].first);
        bool first_inverted = !x.xor_clause_inverted();
        vec<Lit> first_vars;
        process_clause(x, c[0].second, var, first_vars);
        
        #ifdef VERBOSE_DEBUG
        cout << "- Removing: ";
        x.plainPrint();
        cout << "Adding var " << var+1 << " to calcAtFinish" << endl;
        #endif
        
        assert(!toRemove[c[0].second]);
        toRemove[c[0].second] = true;
        S->detachClause(x);
        calcAtFinish.push(&x);
        found++;
        
        for (uint i = 1; i < c.size(); i++) {
            vec<Lit> ps(first_vars.size());
            memcpy(ps.getData(), first_vars.getData(), sizeof(Lit)*first_vars.size());
            XorClause& x = *c[i].first;
            process_clause(x, c[i].second, var, ps);
            
            #ifdef VERBOSE_DEBUG
            cout << "- Removing: ";
            x.plainPrint();
            #endif
            
            const uint old_group = x.getGroup();
            bool inverted = first_inverted ^ x.xor_clause_inverted();
            assert(!toRemove[c[i].second]);
            toRemove[c[i].second] = true;
            S->removeClause(x);
            found++;
            clearDouble(ps);
            
            if (!dealWithNewClause(ps, inverted, old_group)) {
                clearToRemove();
                S->ok = false;
                return found;
            }
        }
        
        varToXor.erase(it);
    }
    
    clearToRemove();
    
    if (S->ok != false)
        S->ok = (S->propagate() == NULL);
    
    return found;
}

bool Conglomerate::dealWithNewClause(vec<Lit>& ps, const bool inverted, const uint old_group)
{
    switch(ps.size()) {
        case 0: {
            #ifdef VERBOSE_DEBUG
            cout << "--> xor is 0-long" << endl;
            #endif
            
            if  (!inverted)
                return false;
            break;
        }
        case 1: {
            #ifdef VERBOSE_DEBUG
            cout << "--> xor is 1-long, attempting to set variable " << ps[0].var()+1 << endl;
            #endif
            
            if (S->assigns[ps[0].var()] == l_Undef) {
                assert(S->decisionLevel() == 0);
                blocked[ps[0].var()] = true;
                S->uncheckedEnqueue(Lit(ps[0].var(), inverted));
            } else if (S->assigns[ps[0].var()] != boolToLBool(!inverted)) {
                #ifdef VERBOSE_DEBUG
                cout << "Conflict. Aborting.";
                #endif
                return false;
            }
            break;
        }
        
        case 2: {
            #ifdef VERBOSE_DEBUG
            cout << "--> xor is 2-long, must later replace variable, adding var " << ps[0].var() + 1 << " to calcAtFinish:" << endl;
            XorClause* newX = XorClause_new(ps, inverted, old_group);
            newX->plainPrint();
            free(newX);
            #endif
            
            S->varReplacer->replace(ps, inverted, old_group);
            blocked[ps[0].var()] = true;
            blocked[ps[1].var()] = true;
            break;
        }
        
        default: {
            XorClause* newX = XorClause_new(ps, inverted, old_group);
            
            #ifdef VERBOSE_DEBUG
            cout << "- Adding: ";
            newX->plainPrint();
            #endif
            
            S->xorclauses.push(newX);
            toRemove.push_back(false);
            S->attachClause(*newX);
            for (const Lit * a = &((*newX)[0]), *end = a + newX->size(); a != end; a++) {
                if (!blocked[a->var()])
                    varToXor[a->var()].push_back(make_pair(newX, (uint32_t)(toRemove.size()-1)));
            }
            break;
        }
    }
    
    return true;
}

void Conglomerate::clearDouble(vec<Lit>& ps) const
{
    std::sort(ps.getData(), ps.getData() + ps.size());
    Lit p;
    uint i, j;
    for (i = j = 0, p = lit_Undef; i < ps.size(); i++) {
        if (ps[i] == p) {
            //added, but easily removed
            j--;
            p = lit_Undef;
        } else //just add
            ps[j++] = p = ps[i];
    }
    ps.shrink(i - j);
}

void Conglomerate::clearToRemove()
{
    assert(toRemove.size() == S->xorclauses.size());
    
    XorClause **a = S->xorclauses.getData();
    XorClause **r = a;
    XorClause **end = a + S->xorclauses.size();
    for (uint i = 0; r != end; i++) {
        if (!toRemove[i])
            *a++ = *r++;
        else {
            (**a).mark(1);
            r++;
        }
    }
    S->xorclauses.shrink(r-a);
    
    clearLearntsFromToRemove();
}

void Conglomerate::clearLearntsFromToRemove()
{
    Clause **a = S->learnts.getData();
    Clause **r = a;
    Clause **end = a + S->learnts.size();
    for (; r != end;) {
        const Clause& c = **r;
        bool inside = false;
        if (!S->locked(c)) {
            for (uint i = 0; i < c.size(); i++) {
                if (removedVars[c[i].var()]) {
                    inside = true;
                    break;
                }
            }
        }
        if (!inside)
            *a++ = *r++;
        else {
            S->removeClause(**r);
            r++;
        }
    }
    S->learnts.shrink(r-a);
}

void Conglomerate::doCalcAtFinish()
{
    #ifdef VERBOSE_DEBUG
    cout << "Executing doCalcAtFinish" << endl;
    #endif
    
    vector<Var> toAssign;
    for (XorClause** it = calcAtFinish.getData() + calcAtFinish.size()-1; it != calcAtFinish.getData()-1; it--) {
        toAssign.clear();
        XorClause& c = **it;
        assert(c.size() > 2);
        
        #ifdef VERBOSE_DEBUG
        cout << "doCalcFinish for xor-clause:";
        c.plainPrint();
        #endif
        
        bool final = c.xor_clause_inverted();
        for (int k = 0, size = c.size(); k < size; k++ ) {
            const lbool& val = S->assigns[c[k].var()];
            if (val == l_Undef)
                toAssign.push_back(c[k].var());
            else
                final ^= val.getBool();
        }
        #ifdef VERBOSE_DEBUG
        if (toAssign.size() == 0) {
            cout << "ERROR: toAssign.size() == 0 !!" << endl;
            for (int k = 0, size = c.size(); k < size; k++ ) {
                cout << "Var: " << c[k].var() + 1 << " Level: " << S->level[c[k].var()] << endl;
            }
        }
        if (toAssign.size() > 1) {
            cout << "Double assign!" << endl;
            for (uint i = 1; i < toAssign.size(); i++) {
                cout << "-> extra Var " << toAssign[i]+1 << endl;
            }
        }
        #endif
        assert(toAssign.size() > 0);
        
        for (uint i = 1; i < toAssign.size(); i++) {
            S->uncheckedEnqueue(Lit(toAssign[i], true), &c);
        }
        S->uncheckedEnqueue(Lit(toAssign[0], final), &c);
        assert(S->clauseCleaner->satisfied(c));
    }
}

void Conglomerate::addRemovedClauses()
{
    #ifdef VERBOSE_DEBUG
    cout << "Executing addRemovedClauses" << endl;
    #endif
    
    char tmp[100];
    tmp[0] = '\0';
    vec<Lit> ps;
    for(uint i = 0; i < calcAtFinish.size(); i++)
    {
        XorClause& c = *calcAtFinish[i];
        #ifdef VERBOSE_DEBUG
        cout << "readding already removed (conglomerated) clause: ";
        c.plainPrint();
        #endif
        
        ps.clear();
        for(uint i2 = 0; i2 != c.size() ; i2++) {
            ps.push(Lit(c[i2].var(), false));
        }
        S->addXorClause(ps, c.xor_clause_inverted(), c.getGroup(), tmp, true);
        free(&c);
    }
    calcAtFinish.clear();
    for (uint i = 0; i < removedVars.size(); i++) {
        if (removedVars[i]) {
            removedVars[i] = false;
            S->setDecisionVar(i, true);
        }
    }
}

void Conglomerate::newVar()
{
    removedVars.resize(removedVars.size()+1, false);
}
